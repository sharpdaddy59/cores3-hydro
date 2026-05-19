// sim_state.h — per-sensor mode (REAL / SIMULATED / DISABLED), NVS-backed.
//
// The values themselves live in g_state.air_mode / water_mode / light_mode
// (see sensors.h, all atomic<uint8_t> backing the SensorMode enum). These
// helpers handle loading from NVS at boot and saving when the user picks
// a new mode via POST /sim.
//
// NVS namespace: "sim". Keys: "air", "water", "light" (uint8: 0=REAL,
// 1=SIMULATED, 2=DISABLED). Existing pre-v0.7.0 installs stored these as
// bools — false (0) maps to REAL and true (1) maps to SIMULATED, so the
// migration is automatic and silent.

#pragma once

// Load all three modes from NVS into g_state. Missing keys default to
// REAL (0). Call once during setup() after M5.begin().
void sim_state_load();

// Save the corresponding mode's current g_state value to NVS. Called
// after each successful POST /sim that changed it. No-op on NVS error.
void sim_state_save_air();
void sim_state_save_water();
void sim_state_save_light();
