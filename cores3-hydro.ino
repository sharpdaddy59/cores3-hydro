// cores3-hydro.ino — boot, FreeRTOS task creation, main loop.
//
// Built with arduino-cli using M5Stack's arduino-esp32 fork
// (m5stack:esp32:m5stack_cores3 FQBN). PlatformIO with mainstream
// arduino-esp32 had unreliable PSRAM init on the CoreS3, which broke
// camera support; M5Stack's fork has the correct board init.
//
// Build:    .\build.ps1                          (compile only)
//           .\build.ps1 -Upload -Monitor         (compile + flash + serial)
//           .\build.ps1 -Env sim -Upload         (simulation env)
// Setup:    .\setup.ps1                          (one-time, installs deps)

#include <Arduino.h>
#include <M5Unified.h>
#include <Wire.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Bump the Arduino loopTask stack from the 8 KB default to 16 KB. Needed
// because wifi_setup_run() (running during setup() on the loopTask) calls
// quirc, whose internal decode pipeline uses a significant amount of stack
// in addition to the static buffers we already moved out of the function.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#include "config.h"
#include "sensors.h"
#include "sim_state.h"
#include "device_name.h"
#include "dht20.h"
#include "ds18b20.h"
#include "light.h"
#include "battery.h"
#include "camera.h"
#include "http_server.h"
#include "net.h"
#include "display.h"

// ---------------------------------------------------------------------------
// Globals (declarations live in sensors.h)
// ---------------------------------------------------------------------------
SensorState        g_state;
SemaphoreHandle_t  g_wire_mutex = nullptr;

uint32_t now_seconds_since_boot() {
  return millis() / 1000;
}

bool reading_is_fresh(uint32_t sensor_ss_boot, uint32_t now_ss_boot) {
  if (sensor_ss_boot == 0) return false;
  return (now_ss_boot - sensor_ss_boot) <= SENSOR_STALE_S;
}

// ---------------------------------------------------------------------------
// Credential reset gesture: long-touch top-right at boot.
// ---------------------------------------------------------------------------
static void check_credential_reset() {
  M5.Display.setCursor(10, 80);
  M5.Display.setTextSize(1);
  M5.Display.println("(touch top-right to reset WiFi)");

  uint32_t deadline    = millis() + CRED_RESET_WINDOW_MS;
  uint32_t in_zone_ms  = 0;

  while (millis() < deadline) {
    M5.update();
    auto t = M5.Touch.getDetail();
    bool in_zone = t.isPressed()
        && t.x >= CRED_RESET_TOUCH_X
        && t.x <  CRED_RESET_TOUCH_X + CRED_RESET_TOUCH_W
        && t.y >= CRED_RESET_TOUCH_Y
        && t.y <  CRED_RESET_TOUCH_Y + CRED_RESET_TOUCH_H;

    if (in_zone) {
      if (in_zone_ms == 0) in_zone_ms = millis();
      if (millis() - in_zone_ms >= CRED_RESET_HOLD_MS) {
        Serial.println("[setup] credential reset gesture detected");
        net_reset_credentials();   // wipes NVS and reboots
        return;                    // unreachable
      }
    } else {
      in_zone_ms = 0;
    }
    delay(20);
  }
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  Serial.begin(115200);
  Serial.println();
  Serial.println("[boot] cores3-hydro " FW_VERSION);

  M5.Display.setRotation(1);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 10);
  M5.Display.println("cores3-hydro " FW_VERSION);
  M5.Display.setCursor(10, 40);
  M5.Display.println("booting...");

  g_wire_mutex = xSemaphoreCreateMutex();
  if (!g_wire_mutex) {
    Serial.println("[boot] FATAL: could not create wire mutex");
    while (true) delay(1000);
  }

  // Load per-sensor simulation overrides from NVS into g_state. Sensor
  // tasks check these flags at runtime to decide between real-hardware
  // reads and simulated values. Toggled at runtime via POST /sim.
  sim_state_load();

  // Compute the per-MAC default hostname and load any user override from
  // NVS. Must run before net_begin() so WiFi.setHostname() / mDNS pick up
  // the right name on first connect.
  device_name_init();

  check_credential_reset();

  // Camera init goes EARLY — before WiFi and other heavy memory consumers
  // fragment PSRAM. The camera frame buffers need a contiguous allocation
  // and we've seen failures (frame buffer malloc failed) when this runs
  // after net_begin(). Right after M5.begin() PSRAM is mostly empty.
  // Camera failure is non-fatal: Wire1 is reclaimed regardless and the
  // rest of the device continues to work.
  camera_start();

  // Bring up WiFi (WiFiManager captive portal on first boot, reconnect monitor after).
  net_begin();

  // Sensor threads own their data. Start them before HTTP so /sensors has values.
  dht20_start();
  ds18b20_start();
  light_start();
  battery_start();

  // Bind app HTTP server. Must run after WiFiManager has released port 80.
  http_server_begin();

  // Optional display loop (no-op if -DDISABLE_DISPLAY).
  display_start();

  Serial.println("[boot] setup complete");
}

void loop() {
  M5.update();
  http_server_loop();   // synchronous WebServer needs handleClient() polling
  net_loop();           // OTA handle + WiFi state mirror
  delay(10);
}
