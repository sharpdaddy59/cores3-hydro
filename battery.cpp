// battery.cpp — AXP2101 PMIC battery-percent poll task.
//
// Updates g_state.battery_pct every BATTERY_POLL_MS. M5.Power.getBatteryLevel()
// returns 0–100 when a LiPo is attached, or a value at or near 0 when running
// on USB power only (depends on what the AXP2101 reports for the open battery
// terminal). We clip to [0,100] before storing.

#include <Arduino.h>
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "battery.h"
#include "sensors.h"
#include "config.h"

static void battery_task(void *param) {
  (void)param;
  for (;;) {
    // Idle during OTA — see dht20.cpp for rationale.
    if (g_state.ota_in_progress.load()) {
      vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_MS));
      continue;
    }

    int level = M5.Power.getBatteryLevel();   // 0..100, or -1 if unsupported
    if (level < 0)   level = 0;
    if (level > 100) level = 100;
    g_state.battery_pct.store(level);
    vTaskDelay(pdMS_TO_TICKS(BATTERY_POLL_MS));
  }
}

void battery_start() {
  xTaskCreatePinnedToCore(battery_task, "battery", 4096, nullptr, 1, nullptr, 1);
}
