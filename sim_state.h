// sim_state.h — per-sensor simulation override flags, persisted to NVS.
//
// The flags themselves live in g_state.simulate_air / simulate_water /
// simulate_light (see sensors.h). These functions handle loading them
// from NVS at boot and saving them when the user toggles via /sim.
//
// NVS namespace: "sim". Keys: "air", "water", "light" (uint8 0/1).

#pragma once

// Load all three flags from NVS into g_state. Missing keys default to false
// (real hardware reads). Call once during setup() after M5.begin().
void sim_state_load();

// Save the corresponding flag's current g_state value to NVS. Called after
// each successful POST /sim that flipped the flag. No-op if NVS write fails.
void sim_state_save_air();
void sim_state_save_water();
void sim_state_save_light();
