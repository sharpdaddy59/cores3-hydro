// wifi_setup.cpp — first-time WiFi setup via tzapu/WiFiManager.
//
// On boot, net_begin() calls wifi_setup_run() unconditionally. If WiFi
// credentials are already persisted (from a previous run), WiFiManager's
// autoConnect() reconnects without opening the portal. If not, it opens
// an open SoftAP and serves a captive portal with a scanned SSID list +
// password form; we surface the AP name and IP on the display so the
// user knows what to connect to.
//
// All credential persistence is handled inside WiFiManager / the
// arduino-esp32 WiFi stack — the project's old `wifi` Preferences
// namespace is no longer in use. wifi_creds_clear() wipes the live
// WiFiManager state and also nukes that legacy namespace so a unit
// upgraded from a QR-setup build doesn't carry stale keys.

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WiFiManager.h>

#include "wifi_setup.h"
#include "config.h"
#include "device_name.h"

// 3-minute timeout for the configuration portal. If the user walks away
// without finishing setup, the device reboots and re-enters the AP next
// boot — better than sitting blocked forever.
static const uint32_t CONFIG_PORTAL_TIMEOUT_S = 180;

// Legacy NVS namespace from the QR-setup era. Cleared on credential
// reset for forward-compatible migration; otherwise unused.
static const char *LEGACY_NVS_NS = "wifi";

// ---------------------------------------------------------------------------
// AP name: "cores3-hydro-setup-<last4mac>". Uses the same per-MAC suffix
// scheme as device_name.cpp so it's recognizable per physical unit.
// ---------------------------------------------------------------------------
static String compute_ap_name() {
  String suffix;
  const char *mac = device_mac();
  size_t mac_len  = (mac != nullptr) ? strlen(mac) : 0;
  if (mac_len >= 4) {
    suffix = String(mac + mac_len - 4);
  } else if (mac_len > 0) {
    suffix = String(mac);
  }
  if (suffix.length() == 0 || suffix == "0000") {
    return String(MDNS_HOSTNAME) + "-setup";
  }
  return String(MDNS_HOSTNAME) + "-setup-" + suffix;
}

// ---------------------------------------------------------------------------
// Display takeover during the portal.
// ---------------------------------------------------------------------------
static void draw_portal_screen(const String &ap_name) {
  M5.Display.fillScreen(TFT_NAVY);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 10);
  M5.Display.println("Wi-Fi Setup");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_YELLOW, TFT_NAVY);
  M5.Display.setCursor(10, 50);
  M5.Display.println("1. Connect to this Wi-Fi:");
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setCursor(20, 68);
  M5.Display.print(ap_name);

  M5.Display.setTextColor(TFT_YELLOW, TFT_NAVY);
  M5.Display.setCursor(10, 95);
  M5.Display.println("2. Open a browser to:");
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setCursor(20, 113);
  M5.Display.println("http://192.168.4.1/");

  M5.Display.setTextColor(TFT_YELLOW, TFT_NAVY);
  M5.Display.setCursor(10, 140);
  M5.Display.println("3. Pick your network + enter");
  M5.Display.setCursor(10, 153);
  M5.Display.println("   the password.");

  M5.Display.setTextColor(TFT_DARKGREY, TFT_NAVY);
  M5.Display.setCursor(10, 195);
  M5.Display.println("Times out after 3 minutes.");
}

static void draw_connecting_screen(const String &ssid) {
  M5.Display.fillScreen(TFT_NAVY);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 10);
  M5.Display.println("Connecting...");
  M5.Display.setTextSize(1);
  M5.Display.setCursor(10, 50);
  M5.Display.print("SSID: ");
  M5.Display.println(ssid.c_str());
}

// ---------------------------------------------------------------------------
// WiFiManager callbacks
// ---------------------------------------------------------------------------
static void on_ap_started(WiFiManager *wm) {
  Serial.printf("[setup] WiFiManager portal up: SSID=\"%s\"  IP=%s\n",
                wm->getConfigPortalSSID().c_str(),
                WiFi.softAPIP().toString().c_str());
}

static void on_save_config() {
  Serial.printf("[setup] WiFiManager saved credentials for SSID=\"%s\"\n",
                WiFi.SSID().c_str());
  draw_connecting_screen(WiFi.SSID());
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool wifi_setup_run() {
  String ap_name = compute_ap_name();
  Serial.printf("[setup] entering WiFi setup (AP=\"%s\")\n", ap_name.c_str());

  draw_portal_screen(ap_name);

  WiFiManager wm;
  wm.setDebugOutput(false);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_S);
  wm.setBreakAfterConfig(true);
  wm.setAPCallback(on_ap_started);
  wm.setSaveConfigCallback(on_save_config);

  // autoConnect: if creds are already saved, this returns true quickly
  // without opening the portal. Otherwise it starts SoftAP + DNS +
  // captive portal HTTP server and blocks until either creds are saved
  // (and STA connected) or the timeout fires.
  bool connected = wm.autoConnect(ap_name.c_str());

  if (connected) {
    Serial.printf("[setup] WiFi connected: IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(),
                  (int)WiFi.RSSI());
  } else {
    Serial.println("[setup] WiFi setup failed or timed out");
  }
  return connected;
}

void wifi_creds_clear() {
  Serial.println("[setup] wiping WiFi credentials");

  // WiFiManager stores creds via the ESP32's native WiFi config; the
  // ergonomic way to clear them is through a WiFiManager instance.
  WiFiManager wm;
  wm.resetSettings();

  // Belt-and-braces: also force the underlying WiFi stack to forget the
  // STA config so the next autoConnect() truly starts fresh.
  WiFi.disconnect(/*wifioff=*/true, /*eraseap=*/true);

  // Migration cleanup: the QR-setup era stored ssid/pass in a project
  // Preferences namespace. Remove it so upgraders don't carry dead keys.
  Preferences prefs;
  if (prefs.begin(LEGACY_NVS_NS, /*readOnly=*/false)) {
    prefs.clear();
    prefs.end();
  }
}
