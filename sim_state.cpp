// sim_state.cpp — NVS-backed persistence for per-sensor sim override flags.

#include <Arduino.h>
#include <Preferences.h>

#include "sim_state.h"
#include "sensors.h"

static const char *NS_SIM = "sim";

void sim_state_load() {
  Preferences prefs;
  if (!prefs.begin(NS_SIM, /*readOnly=*/true)) {
    Serial.println("[sim] no saved sim state; defaulting all to real hardware");
    return;
  }
  bool air   = prefs.getBool("air",   false);
  bool water = prefs.getBool("water", false);
  bool light = prefs.getBool("light", false);
  prefs.end();

  g_state.simulate_air.store(air);
  g_state.simulate_water.store(water);
  g_state.simulate_light.store(light);

  Serial.printf("[sim] loaded: air=%s water=%s light=%s\n",
                air   ? "sim" : "real",
                water ? "sim" : "real",
                light ? "sim" : "real");
}

static void save_one(const char *key, bool value) {
  Preferences prefs;
  if (!prefs.begin(NS_SIM, /*readOnly=*/false)) {
    Serial.printf("[sim] save %s FAILED: NVS open error\n", key);
    return;
  }
  prefs.putBool(key, value);
  prefs.end();
}

void sim_state_save_air()   { save_one("air",   g_state.simulate_air.load());   }
void sim_state_save_water() { save_one("water", g_state.simulate_water.load()); }
void sim_state_save_light() { save_one("light", g_state.simulate_light.load()); }
