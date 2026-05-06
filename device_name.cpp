// device_name.cpp — mDNS hostname management with NVS persistence.

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ctype.h>

#include "device_name.h"
#include "config.h"

static const char *NS_DEVICE = "device";
static const char *KEY_HOST  = "host";

// All three are populated in device_name_init() and only mutated from
// device_hostname_set() (which runs on the HTTP server task — single
// writer). Readers see a stable C-string between writes; updates happen
// before mDNS is re-registered, so the new pointer is what callers see.
static String s_default;     // "cores3-hydro-a1b2"
static String s_override;    // empty if not set
static String s_active;      // s_override if non-empty, else s_default
static String s_mac;         // 12-char hex, no separators

// ---------------------------------------------------------------------------
// Default name: base + last 4 hex chars of the WiFi STA MAC.
// ---------------------------------------------------------------------------
static void compute_default() {
  // WiFi.macAddress() returns "AA:BB:CC:DD:EE:FF" — uppercase, colons.
  // Arduino-ESP32 falls back to esp_efuse_mac_get_default when the WiFi
  // peripheral isn't started yet, so this works pre-net_begin().
  String mac = WiFi.macAddress();
  String compact;
  compact.reserve(12);
  for (size_t i = 0; i < mac.length(); i++) {
    char c = mac[i];
    if (c == ':') continue;
    compact += (char)tolower((unsigned char)c);
  }
  s_mac = compact;

  // Last 4 chars of the MAC (lowercase) → suffix.
  String suffix;
  if (compact.length() >= 4) {
    suffix = compact.substring(compact.length() - 4);
  } else {
    suffix = compact;
  }

  // If the MAC came back as all zeros for any reason, the suffix would be
  // "0000" for every unit and the unique-default goal is defeated. Fall
  // back to the bare base name in that case.
  if (suffix.length() == 0 || suffix == "0000") {
    s_default = MDNS_HOSTNAME;
  } else {
    s_default = String(MDNS_HOSTNAME) + "-" + suffix;
  }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
const char *device_hostname_validate(const char *name) {
  if (name == nullptr) return "name missing";
  size_t len = strlen(name);
  if (len == 0)  return "name is empty";
  if (len > 63)  return "name too long (max 63)";

  for (size_t i = 0; i < len; i++) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z')
           || (c >= '0' && c <= '9')
           || (c == '-');
    if (!ok) return "only a-z, 0-9, and - allowed";
  }
  if (name[0] == '-')        return "must not start with -";
  if (name[len - 1] == '-')  return "must not end with -";

  return nullptr;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void device_name_init() {
  compute_default();

  Preferences prefs;
  String stored;
  if (prefs.begin(NS_DEVICE, /*readOnly=*/true)) {
    stored = prefs.getString(KEY_HOST, "");
    prefs.end();
  }

  // Defensive: if a stored name is somehow corrupt (older firmware, manual
  // NVS edit, whatever), ignore it rather than refusing to boot.
  if (stored.length() > 0) {
    const char *err = device_hostname_validate(stored.c_str());
    if (err) {
      Serial.printf("[device] stored hostname '%s' invalid (%s); ignoring\n",
                    stored.c_str(), err);
      stored = "";
    }
  }

  s_override = stored;
  s_active   = (s_override.length() > 0) ? s_override : s_default;

  Serial.printf("[device] hostname: %s (default %s, mac %s)\n",
                s_active.c_str(), s_default.c_str(), s_mac.c_str());
}

// ---------------------------------------------------------------------------
// Getters
// ---------------------------------------------------------------------------
const char *device_hostname()         { return s_active.c_str(); }
const char *device_hostname_default() { return s_default.c_str(); }
const char *device_mac()              { return s_mac.c_str(); }

// ---------------------------------------------------------------------------
// Setter — empty string clears the override and reverts to default.
// ---------------------------------------------------------------------------
bool device_hostname_set(const char *name) {
  if (name == nullptr) return false;
  String candidate(name);
  candidate.trim();
  // Lowercase up front so 'Greenhouse-1' becomes 'greenhouse-1'.
  for (size_t i = 0; i < candidate.length(); i++) {
    candidate[i] = (char)tolower((unsigned char)candidate[i]);
  }

  // Empty → clear override.
  if (candidate.length() == 0) {
    Preferences prefs;
    if (!prefs.begin(NS_DEVICE, /*readOnly=*/false)) {
      Serial.println("[device] NVS open failed (clear)");
      return false;
    }
    prefs.remove(KEY_HOST);
    prefs.end();

    s_override = "";
    s_active   = s_default;
    Serial.printf("[device] hostname cleared; using default %s\n", s_default.c_str());
    return true;
  }

  const char *err = device_hostname_validate(candidate.c_str());
  if (err) {
    Serial.printf("[device] reject hostname '%s': %s\n", candidate.c_str(), err);
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(NS_DEVICE, /*readOnly=*/false)) {
    Serial.println("[device] NVS open failed (set)");
    return false;
  }
  prefs.putString(KEY_HOST, candidate);
  prefs.end();

  s_override = candidate;
  s_active   = s_override;
  Serial.printf("[device] hostname set to %s\n", s_active.c_str());
  return true;
}
