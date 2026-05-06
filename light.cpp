// light.cpp — LTR-553ALS reader task.
//
// The CoreS3's onboard ambient light sensor sits at I2C address 0x23 on the
// internal bus. Per the spec it operates normally in BOTH simulation and
// production modes — sim mode only fakes out the external Grove sensors,
// not the always-present onboard hardware.
//
// We talk to it through M5.In_I2C rather than raw Wire1. Reasons:
//   - M5.In_I2C automatically serializes against M5Unified's own internal
//     I2C consumers (M5.update touch + IMU reads). Using raw Wire1 races
//     against those, which manifests as i2cWriteReadNonStop Error -1 on
//     repeated-start reads.
//   - The API is one-call read/write helpers, no begin/write/endTransmission
//     ceremony.
//
// Lux formula derived from the LTR-553ALS datasheet, default settings:
//   ALS gain = 1x, integration time = 100 ms, measurement rate = 500 ms.

#include <Arduino.h>
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "light.h"
#include "sensors.h"
#include "config.h"
#include "simulation.h"

// ---------------------------------------------------------------------------
// LTR-553ALS register map (subset)
// ---------------------------------------------------------------------------
static constexpr uint8_t LTR553_ADDR        = 0x23;

static constexpr uint8_t REG_ALS_CONTR      = 0x80;
static constexpr uint8_t REG_ALS_MEAS_RATE  = 0x85;
static constexpr uint8_t REG_PART_ID        = 0x86;
static constexpr uint8_t REG_ALS_DATA_CH1_0 = 0x88;

static constexpr uint8_t LTR553_PART_ID     = 0x92;   // ALS variant

// ---------------------------------------------------------------------------
// Init / read — all bus access via M5.In_I2C (self-locking).
// ---------------------------------------------------------------------------
static bool ltr553_init() {
  uint8_t part_id = 0;
  if (!M5.In_I2C.readRegister(LTR553_ADDR, REG_PART_ID, &part_id, 1, 100000)) {
    Serial.println("[light] failed to read PART_ID register");
    return false;
  }
  Serial.printf("[light] PART_ID=0x%02x\n", part_id);
  if (part_id != LTR553_PART_ID) {
    Serial.printf("[light] unexpected part ID 0x%02x (expected 0x92)\n", part_id);
    return false;
  }
  // ALS_CONTR: gain 1x (000), active mode (1)  -> 0x01
  if (!M5.In_I2C.writeRegister8(LTR553_ADDR, REG_ALS_CONTR, 0x01, 100000)) {
    Serial.println("[light] failed to write ALS_CONTR");
    return false;
  }
  // ALS_MEAS_RATE: 100 ms integration (000), 500 ms measurement rate (011)  -> 0x03
  if (!M5.In_I2C.writeRegister8(LTR553_ADDR, REG_ALS_MEAS_RATE, 0x03, 100000)) {
    Serial.println("[light] failed to write ALS_MEAS_RATE");
    return false;
  }
  Serial.println("[light] LTR-553 initialized");
  return true;
}

static bool ltr553_read_lux(uint16_t &lux_out) {
  uint8_t buf[4];
  if (!M5.In_I2C.readRegister(LTR553_ADDR, REG_ALS_DATA_CH1_0, buf, 4, 100000)) {
    Serial.println("[light] failed to read ALS data registers");
    return false;
  }

  uint16_t ch1 = ((uint16_t)buf[1] << 8) | buf[0];   // IR-filtered visible
  uint16_t ch0 = ((uint16_t)buf[3] << 8) | buf[2];   // visible + IR
  uint32_t total = (uint32_t)ch0 + (uint32_t)ch1;

  Serial.printf("[light] ch0=%u ch1=%u\n", (unsigned)ch0, (unsigned)ch1);

  if (total == 0) { lux_out = 0; return true; }

  float ratio = (float)ch1 / (float)total;
  float lux_f;
  if (ratio < 0.45f)       lux_f = 1.7743f * ch0 + 1.1059f * ch1;
  else if (ratio < 0.64f)  lux_f = 4.2785f * ch0 - 1.9548f * ch1;
  else if (ratio < 0.85f)  lux_f = 0.5926f * ch0 + 0.1185f * ch1;
  else                     lux_f = 0.0f;

  if (lux_f < 0.0f)     lux_f = 0.0f;
  if (lux_f > 65535.0f) lux_f = 65535.0f;
  lux_out = (uint16_t)lux_f;
  return true;
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
static void light_task(void *param) {
  (void)param;
  Serial.println("[light] task starting");

  bool present = ltr553_init();
  Serial.printf("[light] LTR-553 present: %s\n", present ? "yes" : "no");

  // After waking from STANDBY: 10 ms wakeup + 100 ms first integration =
  // ~110 ms until valid data is in the data registers. Be generous.
  if (present) vTaskDelay(pdMS_TO_TICKS(150));

  for (;;) {
    uint16_t lux = 0;
    if (g_state.simulate_light.load()) {
      lux = sim_light();
    } else if (present) {
      ltr553_read_lux(lux);
    }

    g_state.light_lux.store(lux);
    g_state.seconds_since_boot_light.store(now_seconds_since_boot());

    vTaskDelay(pdMS_TO_TICKS(LIGHT_INTERVAL_MS));
  }
}

void light_start() {
  xTaskCreatePinnedToCore(light_task, "light", 4096, nullptr, 2, nullptr, 1);
}
