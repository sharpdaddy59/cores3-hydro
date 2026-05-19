// dht20.cpp — DHT20 reader task.
//
// The DHT20 is internally an AHT20 with a fixed I2C address (0x38) and a
// dead-simple protocol. Inline driver — no external library dependency.
//
// Per-sensor mode (REAL / SIMULATED / DISABLED) lives in g_state.air_mode.
// SIMULATED emits sim_air_temp() / sim_humidity() instead of touching the
// bus. DISABLED skips reads entirely — air_temp and humidity stay NAN and
// the API emits null + status:"disabled". The mode is toggled at runtime
// via POST /sim and persisted in NVS.
//
// Protocol summary (AHT20 datasheet §5.4):
//   Trigger:  W [0xAC, 0x33, 0x00]
//   Wait:     ~75 ms (status bit 7 = 1 means busy)
//   Read:     R 7 bytes: [status, h_hi, h_mid, h_lo|t_hi, t_mid, t_lo, crc]
//   Decode:   raw_h = (b1<<12) | (b2<<4) | (b3>>4)             — 20 bits
//             raw_t = ((b3 & 0x0F)<<16) | (b4<<8) | b5          — 20 bits
//             humidity_pct = raw_h * 100 / 2^20
//             temp_c       = raw_t * 200 / 2^20 - 50

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "dht20.h"
#include "sensors.h"
#include "config.h"
#include "simulation.h"

static constexpr uint8_t DHT20_ADDR = 0x38;

static bool dht20_trigger() {
  Wire.beginTransmission(DHT20_ADDR);
  Wire.write(0xAC);
  Wire.write(0x33);
  Wire.write(0x00);
  return Wire.endTransmission() == 0;
}

static bool dht20_read_result(float &temp_c, float &humidity_pct) {
  uint8_t buf[7];
  if (Wire.requestFrom(DHT20_ADDR, (uint8_t)7) != 7) return false;
  for (int i = 0; i < 7; ++i) {
    if (!Wire.available()) return false;
    buf[i] = Wire.read();
  }
  if (buf[0] & 0x80) return false;   // sensor still busy

  uint32_t raw_h =  ((uint32_t)buf[1] << 12)
                  | ((uint32_t)buf[2] <<  4)
                  | ((uint32_t)buf[3] >>  4);
  uint32_t raw_t =  ((uint32_t)(buf[3] & 0x0F) << 16)
                  | ((uint32_t)buf[4] <<  8)
                  | ((uint32_t)buf[5]);

  humidity_pct = (float)raw_h * (100.0f / 1048576.0f);
  temp_c       = (float)raw_t * (200.0f / 1048576.0f) - 50.0f;
  return true;
}

// One-shot presence probe. Returns true if a device ACKs at 0x38.
static bool dht20_probe() {
  Wire.beginTransmission(DHT20_ADDR);
  return Wire.endTransmission() == 0;
}

static void dht20_task(void *param) {
  (void)param;
  Serial.println("[dht20] task starting");

  // Probe once before entering the loop. If the sensor isn't there, idle the
  // hardware-read path — keep advancing the timestamp so /sensors shows the
  // task is alive, but don't keep hammering an empty I2C address (which can
  // wedge the bus driver after repeated NACKs). Re-probe every 5 min in case
  // the user plugs the unit in later. Note this only matters when the sim
  // override is OFF — in sim mode we never touch the bus.
  bool present = false;
  if (xSemaphoreTake(g_wire_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    present = dht20_probe();
    xSemaphoreGive(g_wire_mutex);
  }
  Serial.printf("[dht20] sensor present: %s\n", present ? "yes" : "no");

  static const uint32_t REPROBE_CYCLES = 30;   // 30 * 10s = 5 min
  uint32_t cycles_since_reprobe = 0;

  for (;;) {
    // Hold off all hardware reads while an OTA upload is in flight. The
    // ps_malloc'd firmware buffer needs every spare byte of PSRAM, and the
    // I2C mutex contention here would just delay the upload's reboot.
    // We still vTaskDelay below so the task continues feeding the WDT.
    if (g_state.ota_in_progress.load()) {
      vTaskDelay(pdMS_TO_TICKS(DHT_INTERVAL_MS));
      continue;
    }

    SensorMode mode = sensor_mode_from(g_state.air_mode.load());
    if (mode == SensorMode::OFF) {
      // Sensor explicitly turned off by the user. Skip the bus access
      // and leave value + timestamp untouched — the /sensors handler
      // reads air_mode and emits null + status:"disabled" regardless of
      // what we'd have stored.
      vTaskDelay(pdMS_TO_TICKS(DHT_INTERVAL_MS));
      continue;
    }

    float t = NAN;
    float h = NAN;

    if (mode == SensorMode::SIMULATED) {
      t = sim_air_temp();
      h = sim_humidity();
    } else if (present) {
      // 1. Brief mutex window to issue the trigger command.
      bool triggered = false;
      if (xSemaphoreTake(g_wire_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        triggered = dht20_trigger();
        xSemaphoreGive(g_wire_mutex);
      }

      // 2. Conversion happens in the sensor — yield to other tasks.
      if (triggered) {
        vTaskDelay(pdMS_TO_TICKS(85));

        // 3. Brief mutex window to read the 7-byte result.
        if (xSemaphoreTake(g_wire_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          float tt, hh;
          if (dht20_read_result(tt, hh)) {
            t = tt;
            h = hh;
          }
          xSemaphoreGive(g_wire_mutex);
        }
      }
    } else if (++cycles_since_reprobe >= REPROBE_CYCLES) {
      cycles_since_reprobe = 0;
      if (xSemaphoreTake(g_wire_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        present = dht20_probe();
        xSemaphoreGive(g_wire_mutex);
        if (present) Serial.println("[dht20] sensor detected on reprobe");
      }
    }

    g_state.air_temp.store(t);
    g_state.humidity.store(h);
    g_state.seconds_since_boot_air.store(now_seconds_since_boot());

    vTaskDelay(pdMS_TO_TICKS(DHT_INTERVAL_MS));
  }
}

void dht20_start() {
  // 8 KB stack — Wire + Serial.printf paths can be deep. Cheap insurance.
  xTaskCreatePinnedToCore(dht20_task, "dht20", 8192, nullptr, 2, nullptr, 1);
}
