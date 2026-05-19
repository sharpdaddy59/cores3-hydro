// ds18b20.cpp — DS18B20 reader task.
//
// The Grove DS18B20 unit used in this build ships with the 4.7k pullup
// integrated, so no external pullup is needed on GPIO 8.
//
// The 750 ms conversion is handled with vTaskDelay() — yielding to the
// scheduler inherently feeds the task watchdog. Do not disable the watchdog.
//
// Per-sensor mode (REAL / SIMULATED / DISABLED) lives in g_state.water_mode.
// SIMULATED emits sim_water_temp() without touching the bus. DISABLED skips
// reads entirely — water_temp stays NAN and the API emits null +
// status:"disabled". Toggled at runtime via POST /sim, persisted in NVS.

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ds18b20.h"
#include "sensors.h"
#include "config.h"
#include "simulation.h"

static OneWire           s_one_wire(PIN_DS18B20_DATA);
static DallasTemperature s_ds(&s_one_wire);

static void ds18b20_task(void *param) {
  (void)param;

  s_ds.begin();
  s_ds.setWaitForConversion(false);   // we manage the wait via vTaskDelay

  for (;;) {
    // Idle during OTA — see dht20.cpp for rationale.
    if (g_state.ota_in_progress.load()) {
      vTaskDelay(pdMS_TO_TICKS(DS18B20_INTERVAL_MS));
      continue;
    }

    SensorMode mode = sensor_mode_from(g_state.water_mode.load());
    if (mode == SensorMode::OFF) {
      // User explicitly turned off the water sensor (e.g. microgreens —
      // no tank). Skip the 800 ms conversion + the 1-Wire transaction;
      // the /sensors handler will emit null + status:"disabled" based on
      // water_mode alone.
      vTaskDelay(pdMS_TO_TICKS(DS18B20_INTERVAL_MS));
      continue;
    }

    float t = NAN;

    if (mode == SensorMode::SIMULATED) {
      t = sim_water_temp();
    } else {
      s_ds.requestTemperatures();
      vTaskDelay(pdMS_TO_TICKS(800));   // conversion window — yields & feeds WDT
      t = s_ds.getTempCByIndex(0);
      if (t == DEVICE_DISCONNECTED_C) t = NAN;
    }

    g_state.water_temp.store(t);
    g_state.seconds_since_boot_water.store(now_seconds_since_boot());

    vTaskDelay(pdMS_TO_TICKS(DS18B20_INTERVAL_MS));
  }
}

void ds18b20_start() {
  xTaskCreatePinnedToCore(ds18b20_task, "ds18b20", 4096, nullptr, 2, nullptr, 1);
}
