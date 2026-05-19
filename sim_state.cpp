// sim_state.cpp — NVS-backed persistence for per-sensor mode (tristate).

#include <Arduino.h>
#include <Preferences.h>

#include "sim_state.h"
#include "sensors.h"

static const char *NS_SIM = "sim";

void sim_state_load() {
  Preferences prefs;
  if (!prefs.begin(NS_SIM, /*readOnly=*/true)) {
    Serial.println("[sim] no saved state; defaulting all sensors to REAL");
    return;
  }
  // getUChar transparently reads pre-v0.7.0 bool keys (false=0=REAL,
  // true=1=SIMULATED), so existing installs migrate without losing their
  // sim toggles.
  uint8_t air   = prefs.getUChar("air",   (uint8_t)SensorMode::REAL);
  uint8_t water = prefs.getUChar("water", (uint8_t)SensorMode::REAL);
  uint8_t light = prefs.getUChar("light", (uint8_t)SensorMode::REAL);
  prefs.end();

  g_state.air_mode.store((uint8_t)sensor_mode_from(air));
  g_state.water_mode.store((uint8_t)sensor_mode_from(water));
  g_state.light_mode.store((uint8_t)sensor_mode_from(light));

  Serial.printf("[sim] loaded: air=%s water=%s light=%s\n",
                sensor_mode_label(sensor_mode_from(air)),
                sensor_mode_label(sensor_mode_from(water)),
                sensor_mode_label(sensor_mode_from(light)));
}

static void save_one(const char *key, uint8_t value) {
  Preferences prefs;
  if (!prefs.begin(NS_SIM, /*readOnly=*/false)) {
    Serial.printf("[sim] save %s FAILED: NVS open error\n", key);
    return;
  }
  prefs.putUChar(key, value);
  prefs.end();
}

void sim_state_save_air()   { save_one("air",   g_state.air_mode.load());   }
void sim_state_save_water() { save_one("water", g_state.water_mode.load()); }
void sim_state_save_light() { save_one("light", g_state.light_mode.load()); }
