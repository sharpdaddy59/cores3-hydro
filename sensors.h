// sensors.h — global sensor state and the I2C bus mutex.
//
// Per the spec: each sensor field has a single writer task, fields are
// std::atomic so cross-core reads are safe, and the I2C bus mutex (g_wire_mutex)
// must be held during any transaction on the internal I2C bus that the
// touch controller, PMIC, RTC, IMU, DHT20, and LTR-553ALS all share.

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct SensorState {
  // Air (DHT20 thread)
  std::atomic<float>    air_temp{NAN};
  std::atomic<float>    humidity{NAN};
  std::atomic<uint32_t> seconds_since_boot_air{0};

  // Water (DS18B20 thread)
  std::atomic<float>    water_temp{NAN};
  std::atomic<uint32_t> seconds_since_boot_water{0};

  // Light (LTR-553ALS, light thread)
  std::atomic<uint16_t> light_lux{0};
  std::atomic<uint32_t> seconds_since_boot_light{0};

  // System
  uint32_t              boot_time_epoch{0};   // set once by NTP, 0 if unsynced
  std::atomic<int>      wifi_rssi{0};
  std::atomic<int>      battery_pct{0};
  std::atomic<bool>     wifi_connected{false};

  // Per-sensor simulation overrides. When true, the sensor task uses
  // simulated values from simulation.cpp instead of reading hardware.
  // Loaded from NVS at boot; toggled at runtime via POST /sim. Default
  // is real-hardware reads (false).
  std::atomic<bool>     simulate_air{false};     // DHT20 (air_temp + humidity)
  std::atomic<bool>     simulate_water{false};   // DS18B20 (water_temp)
  std::atomic<bool>     simulate_light{false};   // LTR-553ALS (light_lux)
};

extern SensorState g_state;

// Internal I2C bus mutex. Acquire before Wire.* calls, release immediately.
extern SemaphoreHandle_t g_wire_mutex;

// Monotonic seconds since the device booted.
uint32_t now_seconds_since_boot();

// True if a per-sensor seconds_since_boot_* timestamp is within SENSOR_STALE_S
// of the current uptime.
bool reading_is_fresh(uint32_t sensor_ss_boot, uint32_t now_ss_boot);
