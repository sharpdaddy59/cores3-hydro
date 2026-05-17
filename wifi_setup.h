// wifi_setup.h — first-time WiFi setup via tzapu/WiFiManager.
//
// On a device with no saved credentials, wifi_setup_run() opens an open
// SoftAP named "cores3-hydro-setup-<last4mac>" and serves WiFiManager's
// captive portal on http://192.168.4.1/. The user picks an SSID from the
// scan list and enters the password; WiFiManager persists the result via
// the ESP32's built-in WiFi config storage and the call returns with
// STA connected. If saved creds exist the portal never opens and we just
// connect.
//
// Credential storage lives inside WiFiManager / arduino-esp32 — the
// project's old `wifi` Preferences namespace is no longer used. The
// `wifi_creds_clear()` entry point also wipes any stale keys from that
// legacy namespace so upgraders don't carry dead data.

#pragma once

#include <Arduino.h>

// Run WiFiManager's autoConnect. Blocks until either a connection is
// established (returns true) or the 3-minute portal timeout fires
// (returns false). The display is taken over with a static "WiFi Setup"
// screen while the portal is open.
bool wifi_setup_run();

// Wipe persisted WiFi credentials (WiFiManager + ESP32 native config)
// and clean up the legacy `wifi` Preferences namespace. Caller is
// responsible for any subsequent reboot.
void wifi_creds_clear();
