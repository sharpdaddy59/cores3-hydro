// simulation.h — random-value generators used when a sensor's mode is
// SensorMode::SIMULATED (g_state.air_mode / water_mode / light_mode).
// Ranges match the spec's §Simulation Mode table.

#pragma once

#include <stdint.h>

float    sim_air_temp();    // 18.0 - 30.0 °C
float    sim_humidity();    // 40 - 80 %
float    sim_water_temp();  // 20.0 - 28.0 °C
uint16_t sim_light();       // 0 - 1000 lux
