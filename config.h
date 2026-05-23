// config.h — central tunables for the CoreS3 hydroponic monitor.
//
// Everything that you might want to adjust without hunting through the rest
// of the codebase lives here. Pin assignments, sensor intervals, timeouts,
// version string.

#pragma once

// -- Identity ---------------------------------------------------------------
#define FW_VERSION       "0.7.1"
// MDNS_HOSTNAME is the *base* default — actual runtime hostname comes from
// device_hostname() in device_name.h, which appends a per-MAC suffix on
// first boot and supports user override (NVS, /hostname endpoint).
#define MDNS_HOSTNAME    "cores3-hydro"

// -- Local timezone for display ---------------------------------------------
// POSIX TZ string. Default: US Central (Dallas), DST per US rules.
// Reference: https://github.com/nayarsystems/posix_tz_db
//   Central:   "CST6CDT,M3.2.0,M11.1.0"
//   Eastern:   "EST5EDT,M3.2.0,M11.1.0"
//   Mountain:  "MST7MDT,M3.2.0,M11.1.0"
//   Pacific:   "PST8PDT,M3.2.0,M11.1.0"
//   UTC:       "UTC0"
#define TIMEZONE_TZ      "CST6CDT,M3.2.0,M11.1.0"

// -- Sensor freshness -------------------------------------------------------
// A reading older than this is reported as null on the HTTP API.
#define SENSOR_STALE_S   120

// -- Sensor intervals (milliseconds) ----------------------------------------
#define DHT_INTERVAL_MS       10000
#define DS18B20_INTERVAL_MS   10000
#define LIGHT_INTERVAL_MS     30000
#define DISPLAY_INTERVAL_MS    1000
#define BATTERY_POLL_MS       30000

// -- Camera -----------------------------------------------------------------
// Real timeout enforced via xQueueReceive; see camera.cpp.
#define CAMERA_TIMEOUT_MS      3000
#define CAMERA_QUEUE_LEN          1

// -- HTTP -------------------------------------------------------------------
#define HTTP_PORT                80

// -- WiFi reconnection backoff (cumulative table is in spec §WiFi Disconnection)
#define WIFI_BACKOFF_1_MS     15000
#define WIFI_BACKOFF_2_MS     30000
#define WIFI_BACKOFF_3_MS     60000
#define WIFI_BACKOFF_4_MS    120000
#define WIFI_BACKOFF_5_MS    240000
#define WIFI_BACKOFF_CAP_MS  300000

// -- Pin assignments --------------------------------------------------------
// DS18B20 lives on Port B, GPIO 8, 1-Wire. The Grove DS18B20 unit used in
// this build has the 4.7k pullup integrated into the harness — no external
// pullup needed.
#define PIN_DS18B20_DATA          8

// -- Credential reset gesture -----------------------------------------------
// Long-touch in the top-right corner during the first 3 s of setup() wipes
// the saved WiFi credentials and reboots into WiFiManager's captive-portal AP.
#define CRED_RESET_TOUCH_X      260
#define CRED_RESET_TOUCH_Y        0
#define CRED_RESET_TOUCH_W       60
#define CRED_RESET_TOUCH_H       60
#define CRED_RESET_HOLD_MS     1500   // how long to hold to count as a reset
#define CRED_RESET_WINDOW_MS   3000   // window after boot in which to listen
