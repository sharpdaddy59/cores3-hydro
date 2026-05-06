// wifi_setup.h — QR-code-based WiFi credential setup.
//
// On a device with no saved credentials, run wifi_setup_run() which takes
// over the display, runs the camera in QR-scan mode with a live preview,
// parses any decoded WIFI:T:..;S:..;P:..;; payload, and saves the result
// to NVS via the wifi_creds_save() helper below.

#pragma once

#include <Arduino.h>

struct WifiCreds {
  String ssid;
  String password;
};

// Run the QR-based WiFi setup loop. Blocks. Returns true on success
// (creds populated in *out and persisted to NVS). Returns false if the
// user aborts (touch-and-hold the bottom-right corner) or a non-recoverable
// error occurs.
//
// Pre-req: camera_start() must have been called and must have succeeded.
// Side effects: takes exclusive use of the display until it returns.
bool wifi_setup_run(WifiCreds *out);

// NVS-backed credential storage (uses arduino-esp32 Preferences in the
// "wifi" namespace).
bool wifi_creds_load(WifiCreds *out);    // true if both ssid and pass present
bool wifi_creds_save(const WifiCreds *creds);
void wifi_creds_clear();
