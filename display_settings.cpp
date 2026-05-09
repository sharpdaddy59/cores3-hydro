// display_settings.cpp — NVS persistence for display brightness + idle timeouts.

#include <Arduino.h>
#include <Preferences.h>

#include "display_settings.h"
#include "sensors.h"

static const char *NS_DISPLAY = "display";
static const char *KEY_BRIGHT = "bright";
static const char *KEY_DIM    = "dim_ms";
static const char *KEY_SLEEP  = "sleep_ms";

void display_settings_load() {
  // Seed the wake timestamp now so the dim/sleep timers count from boot,
  // not from millis() == 0 (which would dim/sleep instantly on first tick).
  g_state.last_touch_ms.store(millis());

  Preferences prefs;
  if (!prefs.begin(NS_DISPLAY, /*readOnly=*/true)) {
    Serial.println("[display] no saved settings; using defaults");
    return;
  }
  uint8_t  bright   = prefs.getUChar(KEY_BRIGHT, g_state.display_brightness.load());
  uint32_t dim_ms   = prefs.getUInt(KEY_DIM,    g_state.display_dim_ms.load());
  uint32_t sleep_ms = prefs.getUInt(KEY_SLEEP,  g_state.display_sleep_ms.load());
  prefs.end();

  g_state.display_brightness.store(bright);
  g_state.display_dim_ms.store(dim_ms);
  g_state.display_sleep_ms.store(sleep_ms);

  Serial.printf("[display] loaded: brightness=%u dim_ms=%lu sleep_ms=%lu\n",
                (unsigned)bright,
                (unsigned long)dim_ms,
                (unsigned long)sleep_ms);
}

static bool open_rw(Preferences &prefs, const char *key_for_log) {
  if (!prefs.begin(NS_DISPLAY, /*readOnly=*/false)) {
    Serial.printf("[display] save %s FAILED: NVS open error\n", key_for_log);
    return false;
  }
  return true;
}

void display_settings_save_brightness() {
  Preferences prefs;
  if (!open_rw(prefs, KEY_BRIGHT)) return;
  prefs.putUChar(KEY_BRIGHT, g_state.display_brightness.load());
  prefs.end();
}

void display_settings_save_dim_ms() {
  Preferences prefs;
  if (!open_rw(prefs, KEY_DIM)) return;
  prefs.putUInt(KEY_DIM, g_state.display_dim_ms.load());
  prefs.end();
}

void display_settings_save_sleep_ms() {
  Preferences prefs;
  if (!open_rw(prefs, KEY_SLEEP)) return;
  prefs.putUInt(KEY_SLEEP, g_state.display_sleep_ms.load());
  prefs.end();
}
