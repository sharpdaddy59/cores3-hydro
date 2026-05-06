// device_name.h — runtime mDNS hostname, with NVS-persisted user override.
//
// Default-with-MAC-suffix on first boot:
//   "cores3-hydro-a1b2"      (last 4 hex chars of WiFi STA MAC)
//
// User can override via POST /hostname?name=<label> (see http_server.cpp).
// The override is persisted in NVS and survives reboot/reflash.
//
// NVS namespace: "device". Key: "host" (string).
//
// Validation rules (RFC-1035-ish, applied to the user override):
//   - lowercase letters a-z, digits 0-9, hyphen (-)
//   - must not start or end with hyphen
//   - length 1..63
//   - empty string is treated as "reset to default"
//
// All getters return a stable C-string pointer for the lifetime of the
// process — they refer to a static String inside device_name.cpp. The
// string is updated atomically when device_hostname_set() is called.

#pragma once

// Initialize: load override from NVS (if any), compute the per-MAC default,
// and pick the active hostname. Call once during setup() AFTER M5.begin()
// (we need WiFi.macAddress() which is available after M5.begin's WiFi init).
void device_name_init();

// Active hostname (the bare label, NOT the .local FQDN).
// Returns the user override if set, otherwise the per-MAC default.
const char *device_hostname();

// Default hostname for this device (cores3-hydro-<last4mac>). Stable for
// the life of the device — same physical unit always gets the same default.
const char *device_hostname_default();

// MAC address as a string (12 hex chars, no separators), useful for the
// /hostname JSON response so the user can disambiguate physical units.
const char *device_mac();

// Validate a candidate name. Returns nullptr if valid, else a short
// human-readable error string suitable for echoing in a 400 response.
// Empty input is rejected here; treat empty separately as "reset to default".
const char *device_hostname_validate(const char *name);

// Persist a new user-override hostname. Pass an empty string to clear the
// override and revert to the per-MAC default. Validates first; returns
// false if the candidate is invalid or NVS write fails. On success, the
// active hostname is updated immediately and any subsequent device_hostname()
// call returns the new value. Caller is responsible for re-registering mDNS.
bool device_hostname_set(const char *name);
