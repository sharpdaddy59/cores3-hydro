// simulation.cpp — uniform random in the spec's defined ranges.
//
// Called from the sensor tasks when the per-sensor simulation override is
// set in g_state. Toggled at runtime via POST /sim, persisted in NVS.

#include "simulation.h"

#include <stdlib.h>

static float frand_in(float lo, float hi) {
  float u = (float)rand() / (float)RAND_MAX;
  return lo + u * (hi - lo);
}

float    sim_air_temp()   { return frand_in(18.0f, 30.0f); }
float    sim_humidity()   { return frand_in(40.0f, 80.0f); }
float    sim_water_temp() { return frand_in(20.0f, 28.0f); }
uint16_t sim_light()      { return (uint16_t)frand_in(0.0f, 1000.0f); }
