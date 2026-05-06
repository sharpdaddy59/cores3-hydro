// display.cpp — glanceable dashboard task, double-buffered via M5Canvas.
//
// Drawing pattern:
//   1. Clear and draw the entire frame into an off-screen M5Canvas sprite.
//   2. Push the canvas to the real display in a single blit.
//
// This eliminates the visible flicker from clear-then-draw on the live
// display. The 320x240 16-bpp sprite is ~150 KB, allocated in PSRAM.

#include <Arduino.h>
#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <ctime>      // localtime_r, strftime — for the time/date row

#include "display.h"
#include "sensors.h"
#include "config.h"
#include "net.h"
#include "device_name.h"   // device_hostname() — header URL reflects renames

#ifndef DISABLE_DISPLAY

static M5Canvas s_canvas(&M5.Display);

// ---------------------------------------------------------------------------
// OTA-in-progress takeover screen. Painted instead of the dashboard while
// g_state.ota_in_progress is set. Intentionally minimal — no live stats,
// no animation — because the upload path is grabbing PSRAM and we don't
// want the display task pulling in any extra work.
// ---------------------------------------------------------------------------
static void draw_ota_frame() {
  s_canvas.fillScreen(TFT_NAVY);
  s_canvas.setTextColor(TFT_WHITE, TFT_NAVY);

  s_canvas.setTextSize(3);
  s_canvas.setCursor(40, 60);
  s_canvas.print("OTA UPDATE");

  s_canvas.setTextSize(2);
  s_canvas.setCursor(20, 110);
  s_canvas.print("uploading firmware");
  s_canvas.setCursor(20, 135);
  s_canvas.print("to PSRAM buffer...");

  s_canvas.setTextSize(1);
  s_canvas.setCursor(20, 180);
  s_canvas.setTextColor(TFT_YELLOW, TFT_NAVY);
  s_canvas.print("Sensors paused. Device will");
  s_canvas.setCursor(20, 195);
  s_canvas.print("reboot when flash is written.");

  s_canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// One full frame, drawn into the off-screen canvas.
// ---------------------------------------------------------------------------
static void draw_frame() {
  s_canvas.fillScreen(TFT_BLACK);
  s_canvas.setTextSize(2);

  int y = 10;

  // Header — show the mDNS URL so users know where to point a browser.
  // Cyan to suggest it's interactive ("type me into a browser").
  s_canvas.setCursor(10, y);
  s_canvas.setTextColor(TFT_CYAN, TFT_BLACK);
  s_canvas.printf("%s.local", device_hostname());
  y += 32;

  // Simulated sensors get a yellow tint and a "*" suffix so the operator
  // can see at a glance what's real and what's faked.
  bool sim_water = g_state.simulate_water.load();
  bool sim_air   = g_state.simulate_air.load();
  bool sim_light = g_state.simulate_light.load();

  // Water
  s_canvas.setCursor(10, y);
  float wt = g_state.water_temp.load();
  if (std::isnan(wt)) {
    s_canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    s_canvas.printf("WATER  --.-C  --");
  } else if (sim_water) {
    s_canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_canvas.printf("WATER  %4.1fC  *SIM", wt);
  } else {
    s_canvas.setTextColor(wt > 26.0f ? TFT_RED : TFT_CYAN, TFT_BLACK);
    s_canvas.printf("WATER  %4.1fC  OK", wt);
  }
  y += 30;

  // Air + humidity
  s_canvas.setCursor(10, y);
  float at = g_state.air_temp.load();
  float hu = g_state.humidity.load();
  if (std::isnan(at) || std::isnan(hu)) {
    s_canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    s_canvas.printf("AIR    --.-C  --%%");
  } else if (sim_air) {
    s_canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_canvas.printf("AIR    %4.1fC  %2d%% *", at, (int)hu);
  } else {
    s_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    s_canvas.printf("AIR    %4.1fC  %2d%%", at, (int)hu);
  }
  y += 30;

  // Light
  s_canvas.setCursor(10, y);
  if (sim_light) {
    s_canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
    s_canvas.printf("LIGHT  %4u lx *", (unsigned)g_state.light_lux.load());
  } else {
    s_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    s_canvas.printf("LIGHT  %4u lx", (unsigned)g_state.light_lux.load());
  }
  y += 30;

  // WiFi
  s_canvas.setCursor(10, y);
  bool wifi_up = net_is_connected();
  s_canvas.setTextColor(wifi_up ? TFT_WHITE : TFT_DARKGREY, TFT_BLACK);
  s_canvas.printf("WiFi  %4d dBm  %s",
                  net_rssi(),
                  wifi_up ? "OK" : "--");
  y += 30;

  // Battery
  s_canvas.setCursor(10, y);
  int bat = g_state.battery_pct.load();
  if (bat <= 0) {
    s_canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    s_canvas.printf("BAT    USB");
  } else {
    s_canvas.setTextColor(bat < 20 ? TFT_RED : TFT_GREEN, TFT_BLACK);
    s_canvas.printf("BAT    %3d%%", bat);
  }
  y += 30;

  // Time / date — proves NTP synced and the local clock is current.
  // configTzTime() in net.cpp set the timezone, so localtime_r returns
  // values in the local zone (Central by default per config.h TIMEZONE_TZ).
  s_canvas.setCursor(10, y);
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  if (timeinfo.tm_year >= 120) {              // year >= 2020 → clock is set
    char buf[24];
    strftime(buf, sizeof(buf), "%m/%d %I:%M:%S %p", &timeinfo);
    s_canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    s_canvas.printf("%s", buf);
  } else {
    s_canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    s_canvas.printf("--/-- --:--:-- --");
  }

  // Single blit to the live display — no flicker.
  s_canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------
static void display_task(void *param) {
  (void)param;

  // Allocate the off-screen canvas in PSRAM (320x240x2 = ~150 KB).
  s_canvas.setPsram(true);
  s_canvas.setColorDepth(16);
  if (!s_canvas.createSprite(M5.Display.width(), M5.Display.height())) {
    Serial.println("[display] FATAL: could not allocate canvas sprite");
    vTaskDelete(nullptr);
    return;
  }

  for (;;) {
    if (g_state.ota_in_progress.load()) {
      draw_ota_frame();
    } else {
      draw_frame();
    }
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_INTERVAL_MS));
  }
}

void display_start() {
  xTaskCreatePinnedToCore(display_task, "display", 4096, nullptr, 1, nullptr, 1);
}

#else  // DISABLE_DISPLAY

void display_start() { /* no-op */ }

#endif
