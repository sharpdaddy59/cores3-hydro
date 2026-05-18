// ds18b20.cpp — DS18B20 reader task.
//
// The Grove DS18B20 unit used in this build ships with the 4.7k pullup
// integrated, so no external pullup is needed on GPIO 8.
//
// The 750 ms conversion is handled with vTaskDelay() — yielding to the
// scheduler inherently feeds the task watchdog. Do not disable the watchdog.
//
// Per-sensor simulation override: if g_state.simulate_water is true, the
// task emits sim_water_temp() instead of reading hardware. Toggled at
// runtime via POST /sim, persisted in NVS.

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

    float t = NAN;

    if (g_state.simulate_water.load()) {
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
