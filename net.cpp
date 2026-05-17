// net.cpp — WiFi, mDNS, OTA, NTP plumbing.
//
// First boot: tzapu/WiFiManager opens an open SoftAP named
// "cores3-hydro-setup-<last4mac>" and serves a captive portal at
// http://192.168.4.1/. The user picks an SSID and enters the password;
// WiFiManager persists creds via the ESP32's native WiFi config and
// connects in STA mode before returning.
//
// After that: a WiFi event handler tracks STA_GOT_IP and STA_DISCONNECTED.
// Auto-reconnect is disabled — we manage reconnection explicitly with the
// exponential backoff schedule from the spec.
//
// Every successful (re)connect re-initializes mDNS and OTA, and triggers an
// NTP sync. boot_time_epoch is set once on the first successful sync.

#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <time.h>

#include "net.h"
#include "config.h"
#include "sensors.h"
#include "device_name.h"  // device_hostname() — runtime mDNS name
#include "wifi_setup.h"   // WiFiManager captive-portal entry point

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
enum class WifiPhase {
  Boot,           // before WiFiManager.autoConnect()
  Connected,      // STA up
  Disconnected,   // STA down, waiting on backoff window
};

static WifiPhase  s_phase                 = WifiPhase::Boot;
static int        s_attempt               = 0;
static uint32_t   s_next_reconnect_ms     = 0;

static volatile bool s_services_pending   = false;   // set in event handler

static bool       s_ota_started           = false;
static bool       s_ntp_synced            = false;
static uint32_t   s_last_ntp_attempt_ms   = 0;

static const uint32_t NTP_RESYNC_INTERVAL_MS  = 24UL * 60UL * 60UL * 1000UL;
static const uint32_t NTP_RETRY_INTERVAL_MS   = 5UL * 60UL * 1000UL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint32_t backoff_for_attempt(int n) {
  switch (n) {
    case 1:  return WIFI_BACKOFF_1_MS;
    case 2:  return WIFI_BACKOFF_2_MS;
    case 3:  return WIFI_BACKOFF_3_MS;
    case 4:  return WIFI_BACKOFF_4_MS;
    case 5:  return WIFI_BACKOFF_5_MS;
    default: return WIFI_BACKOFF_CAP_MS;
  }
}

static void try_ntp_sync() {
  s_last_ntp_attempt_ms = millis();
  // configTzTime sets the POSIX TZ env var so localtime_r() / getLocalTime()
  // return values in the configured local timezone. boot_time_epoch is still
  // stored as Unix epoch (UTC) — the agent should treat it as such.
  configTzTime(TIMEZONE_TZ, "pool.ntp.org", "time.nist.gov");

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) {
    Serial.println("[net] NTP sync timed out");
    return;
  }

  time_t now;
  time(&now);

  if (g_state.boot_time_epoch == 0) {
    g_state.boot_time_epoch = (uint32_t)now - now_seconds_since_boot();
  }
  s_ntp_synced = true;
  Serial.printf("[net] NTP synced; epoch=%lu boot_epoch=%u (local TZ=%s)\n",
                (unsigned long)now,
                (unsigned)g_state.boot_time_epoch,
                TIMEZONE_TZ);
}

static void start_services() {
  // mDNS: must be fully restarted on each reconnect (spec §Network Service
  // Re-init on Reconnect). The runtime hostname comes from device_name —
  // user override (NVS) takes precedence over the per-MAC default.
  const char *host = device_hostname();
  MDNS.end();
  if (MDNS.begin(host)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    Serial.printf("[net] mDNS up: http://%s.local\n", host);
  } else {
    Serial.println("[net] mDNS begin FAILED");
  }

  // OTA: same restart-on-reconnect story (depends on mDNS for discovery).
  ArduinoOTA.setHostname(host);
  ArduinoOTA.begin();
  s_ota_started = true;
  Serial.println("[net] OTA up");

  // NTP: try on every reconnect until first sync, then on the long cycle.
  if (!s_ntp_synced) {
    try_ntp_sync();
  }
}

// ---------------------------------------------------------------------------
// WiFi event handler — runs in the WiFi event task. Keep work here minimal:
// flip flags and let net_loop() do anything heavy.
// ---------------------------------------------------------------------------
static void on_wifi_event(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      g_state.wifi_connected.store(true);
      g_state.wifi_rssi.store(WiFi.RSSI());
      s_phase            = WifiPhase::Connected;
      s_attempt          = 0;
      s_services_pending = true;
      Serial.printf("[net] connected: %s  (rssi %d)\n",
                    WiFi.localIP().toString().c_str(),
                    (int)WiFi.RSSI());
      break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      g_state.wifi_connected.store(false);
      // Only initiate the backoff machine if we were actually connected;
      // ignore spurious disconnects during portal/setup.
      if (s_phase == WifiPhase::Connected) {
        s_phase             = WifiPhase::Disconnected;
        s_attempt           = 1;
        s_next_reconnect_ms = millis() + backoff_for_attempt(1);
        Serial.printf("[net] disconnected; retry in %lu s\n",
                      (unsigned long)(backoff_for_attempt(1) / 1000));
      }
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void net_begin() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(device_hostname());
  WiFi.setAutoReconnect(false);     // explicit reconnect machine below
  WiFi.onEvent(on_wifi_event);

  // First boot: WiFiManager opens its captive portal (open SoftAP at
  // 192.168.4.1) and blocks until the user picks an SSID + enters a
  // password. Subsequent boots: the same call sees stored credentials
  // and just reconnects in STA mode without opening the portal. Either
  // way, on `true` return WiFi is up; on `false` (portal timeout) we
  // reboot and try again.
  if (!wifi_setup_run()) {
    Serial.println("[net] WiFi setup did not complete; rebooting");
    M5.Display.fillScreen(TFT_NAVY);
    M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 10);
    M5.Display.println("Setup timed out");
    M5.Display.setTextSize(1);
    M5.Display.setCursor(10, 50);
    M5.Display.println("Rebooting to retry...");
    delay(2500);
    ESP.restart();
  }
  // GOT_IP has fired; event handler flipped s_phase to Connected.
}

void net_loop() {
  // 1. Service bring-up after each (re)connect.
  if (s_services_pending) {
    s_services_pending = false;
    start_services();
  }

  // 2. Reconnection state machine.
  if (s_phase == WifiPhase::Disconnected && (int32_t)(millis() - s_next_reconnect_ms) >= 0) {
    Serial.printf("[net] reconnect attempt %d\n", s_attempt);
    WiFi.reconnect();
    // Schedule the next attempt; if reconnect succeeds, GOT_IP will reset
    // s_attempt to 0 before this matters.
    s_attempt++;
    s_next_reconnect_ms = millis() + backoff_for_attempt(s_attempt);
  }

  // 3. NTP cadence — retry every 5 min until first sync, then 24 h.
  if (s_phase == WifiPhase::Connected) {
    uint32_t interval = s_ntp_synced ? NTP_RESYNC_INTERVAL_MS : NTP_RETRY_INTERVAL_MS;
    if ((millis() - s_last_ntp_attempt_ms) > interval) {
      try_ntp_sync();
    }
  }

  // 4. OTA polling.
  if (s_ota_started) ArduinoOTA.handle();

  // 5. RSSI mirror — cheap, do it whenever we're up.
  if (s_phase == WifiPhase::Connected) {
    g_state.wifi_rssi.store(WiFi.RSSI());
  }
}

void net_reset_credentials() {
  Serial.println("[net] wiping WiFi credentials and rebooting");
  wifi_creds_clear();
  delay(200);
  ESP.restart();
}

bool net_is_connected() { return g_state.wifi_connected.load(); }
int  net_rssi()         { return g_state.wifi_rssi.load(); }

void net_apply_hostname_change() {
  // Called after device_hostname_set() persists a new name. mDNS can be
  // re-announced live; WiFi.setHostname() and ArduinoOTA.setHostname()
  // are best-effort here (DHCP-side hostname only fully changes on next
  // reconnect, but the user-facing .local name updates instantly).
  if (!g_state.wifi_connected.load()) {
    Serial.println("[net] hostname change deferred — WiFi not connected");
    return;
  }
  const char *host = device_hostname();
  WiFi.setHostname(host);
  MDNS.end();
  if (MDNS.begin(host)) {
    MDNS.addService("http", "tcp", HTTP_PORT);
    Serial.printf("[net] mDNS re-announced: http://%s.local\n", host);
  } else {
    Serial.println("[net] mDNS re-begin FAILED");
  }
  ArduinoOTA.setHostname(host);
  // ArduinoOTA.begin() was already called from start_services(); re-calling
  // it isn't documented to be safe, so we just update the label. The new
  // OTA name will be fully effective on next reboot.
}
