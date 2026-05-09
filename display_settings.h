// display_settings.h — NVS-backed persistence for display power settings.
//
// The values themselves live in g_state.display_brightness /
// display_dim_ms / display_sleep_ms (see sensors.h). These functions
// handle loading them from NVS at boot and saving them when the user
// changes any of them via POST /display.
//
// NVS namespace: "display". Keys: "bright" (uint8 0..255), "dim_ms"
// (uint32, 0=disabled), "sleep_ms" (uint32, 0=disabled).

#pragma once

#include <cstdint>

// Load settings from NVS into g_state. Missing keys fall back to the
// in-struct defaults (180 / 30000 / 300000). Also seeds last_touch_ms
// with millis() so the display starts in ACTIVE for the full dim window
// after boot. Call once during setup() before display_start().
void display_settings_load();

// Persist the corresponding g_state field's current value to NVS. Called
// after each successful POST /display that changed it. No-op on NVS open
// failure (logged to Serial).
void display_settings_save_brightness();
void display_settings_save_dim_ms();
void display_settings_save_sleep_ms();
