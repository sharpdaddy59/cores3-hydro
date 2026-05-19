// units.cpp — NVS-backed persistence of the temperature unit preference.

#include <Arduino.h>
#include <Preferences.h>

#include <atomic>
#include <cmath>

#include "units.h"

static const char *NS_UNITS = "units";
static const char *KEY_TEMP = "temp";

// Default: Fahrenheit. Matches the display's pre-v0.7.0 behavior so
// existing installs see no visible change at the screen on upgrade.
static std::atomic<uint8_t> s_unit{(uint8_t)TempUnit::FAHRENHEIT};

void units_load() {
  Preferences prefs;
  if (!prefs.begin(NS_UNITS, /*readOnly=*/true)) {
    Serial.println("[units] no saved preference; defaulting to Fahrenheit");
    return;
  }
  uint8_t v = prefs.getUChar(KEY_TEMP, (uint8_t)TempUnit::FAHRENHEIT);
  prefs.end();
  if (v > (uint8_t)TempUnit::FAHRENHEIT) v = (uint8_t)TempUnit::FAHRENHEIT;
  s_unit.store(v);
  Serial.printf("[units] loaded: temperature_units=%s\n", temp_unit_label());
}

TempUnit temp_unit() {
  return (TempUnit)s_unit.load();
}

bool temp_unit_set(TempUnit u) {
  s_unit.store((uint8_t)u);
  Preferences prefs;
  if (!prefs.begin(NS_UNITS, /*readOnly=*/false)) {
    Serial.println("[units] save FAILED: NVS open error");
    return false;
  }
  prefs.putUChar(KEY_TEMP, (uint8_t)u);
  prefs.end();
  return true;
}

const char *temp_unit_label() {
  return temp_unit() == TempUnit::FAHRENHEIT ? "fahrenheit" : "celsius";
}

float temp_in_user_unit(float celsius) {
  if (std::isnan(celsius)) return celsius;
  if (temp_unit() == TempUnit::FAHRENHEIT) {
    return celsius * 9.0f / 5.0f + 32.0f;
  }
  return celsius;
}
