// light.h — built-in LTR-553ALS ambient light reader.
// Internal I2C bus, 30-second cadence, shares g_wire_mutex.

#pragma once

void light_start();
