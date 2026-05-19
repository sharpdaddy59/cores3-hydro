// units.h — user-selectable temperature unit (Celsius / Fahrenheit).
//
// The unit affects everything the user sees and everything the agent
// reads: display rendering and the /sensors JSON both emit temperatures
// in the selected unit. The atomics in g_state still hold raw Celsius
// (that's what the hardware drivers natively return) — conversion
// happens only at the read sites (display.cpp, http_server.cpp).
//
// NVS namespace: "units". Key: "temp" (uint8: 0 = Celsius, 1 = Fahrenheit).
// Default on a fresh device: Fahrenheit, matching how the user already
// runs the display.

#pragma once

#include <cstdint>

enum class TempUnit : uint8_t {
  CELSIUS    = 0,
  FAHRENHEIT = 1,
};

// Load the saved unit preference from NVS. Call once during setup()
// after M5.begin(). Missing key falls back to FAHRENHEIT.
void units_load();

// Current preference. Lock-free; safe to call from any task.
TempUnit temp_unit();

// Set + persist. Returns false only if the NVS write fails (the
// in-memory value is still updated either way).
bool temp_unit_set(TempUnit u);

// "celsius" or "fahrenheit" — used as the JSON `temperature_units` field.
const char *temp_unit_label();

// Convert a raw Celsius reading (NAN passes through) into whatever unit
// the user has selected. Use at every site that emits a temperature
// to the user or to the API.
float temp_in_user_unit(float celsius);
