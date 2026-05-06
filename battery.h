// battery.h — periodic battery percent poll via the AXP2101 PMIC.
//
// The PMIC lives on the internal I2C bus and is read through M5Unified's
// M5.Power facade, which has its own internal locking. This task does NOT
// need g_wire_mutex.

#pragma once

void battery_start();
