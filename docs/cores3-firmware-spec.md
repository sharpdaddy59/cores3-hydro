# CoreS3 Firmware Specification — Hydroponic Monitor (v5)

> **v5 changes:** `MDNS_HOSTNAME` is now a *base default*, not the runtime
> name. On first boot the device computes
> `cores3-hydro-<last4mac>` (e.g. `cores3-hydro-a1b2`) so that two units on
> the same LAN are uniquely discoverable out of the box. The user can
> override the name from the home page or via the new `GET`/`POST /hostname`
> endpoints; the override is persisted in NVS (`device` namespace, key
> `host`) and applied at runtime by re-announcing mDNS without a reboot.
> `device_name.h/.cpp` is the new module that owns this. `FW_VERSION` bumps
> to `0.2.0`.
>
> **v4 changes:** camera now functional and documented as such (the M5.In_I2C
> 100 kHz coexistence pattern + sensor tuning resolved the bus-conflict and
> exposure issues v3 described as blocking); replaced WiFiManager captive
> portal with QR-code-based setup via the camera; rewrote build-system
> guidance from PlatformIO to arduino-cli with M5Stack's arduino-esp32 fork
> (PIO didn't reliably initialize OPI PSRAM on the CoreS3); added
> `GET /` landing page and `POST /wifi/reset` endpoint; `/status` now exposes
> camera, PSRAM, and battery diagnostics; corrected GPIO 2 / Port A note
> (pin_xclk = -1 means no conflict); files section reflects flat
> arduino-cli sketch layout with vendored quirc; **simulation is now a
> per-sensor runtime override** (`GET`/`POST /sim`, NVS-persisted) instead
> of a compile-time flag — `/sensors` exposes a `simulated` block so the
> agent can tell real from fake; the `cores3-sim` build env is removed.
>
> **v3 changes:** dropped seqlock fence pattern (unnecessary on 32-bit atomics),
> rewrote camera timeout strategy (polling loop didn't actually time out),
> added mDNS/OTA reconnect handling, switched IDE recommendation to PlatformIO
> + VS Code, pinned library versions, added PSRAM config, ArduinoJson and
> credential-reset notes, tightened watchdog guidance, updated files layout for
> PlatformIO, added `config.h` for tunables.
>
> **v2 changes:** concurrency model (std::atomic + mutex), fixed bus topology,
> camera strategy (init-once), simplified timestamp scheme, boot time realism,
> display font clarity, pump dead-man noted as sole exception.

## Overview

The CoreS3 runs a single-purpose firmware: read sensors on independent threads,
serve the latest values via HTTP, and optionally update the built-in display.
The CoreS3 is a stateless data source — it never interprets readings, never
alerts, never makes decisions. All reasoning happens in the upstream agent.

**Sole exception:** The pump safety dead-man (see Future section) is a
hardware-safety mechanism, not a normal control path. It is the only decision
the firmware makes, and only when the agent is unreachable.

## Architecture

```
┌─────────────────────────────────┐
│          CoreS3 Firmware         │
│                                 │
│  ┌──────────┐  ┌──────────┐    │
│  │ DHT20    │  │ DS18B20  │    │  ← Sensor threads (independent)
│  │ Thread   │  │ Thread   │    │
│  └────┬─────┘  └────┬─────┘    │
│       │             │          │  ← I2C bus shared with touch, PMIC,
│       │             │             RTC, IMU — Wire mutex required
│       ▼             ▼          │
│  ┌─────────────────────────┐   │
│  │    Global Sensor State   │   │  ← std::atomic per field
│  │    (atomic, single-writer)│  │     value+ts written coherently
│  └──────────┬──────────────┘   │
│             │                  │
│  ┌──────────▼──────────────┐   │
│  │     HTTP Server          │   │  ← Main thread, reads globals only
│  │     /sensors             │   │
│  │     /status              │   │
│  │     /snapshot            │   │
│  └─────────────────────────┘   │
│                                 │
│  ┌──────────┐  ┌──────────┐    │
│  │ Display  │  │ Camera   │    │  ← camera init'd once at boot
│  │ Loop     │  │          │    │     fb_get in sync WebServer handler
│  └──────────┘  └──────────┘    │
└─────────────────────────────────┘
```

**Key principle:** sensor threads own their data. The HTTP handler is never
blocked waiting for a sensor. A stale reading is acceptable — the agent's trend
analysis will catch extended freezes.

## Simulation Mode

Each sensor independently runs in either **real-hardware** mode (the default)
or **simulated** mode (random values from `simulation.cpp`). The flag for
each sensor is stored in `g_state.simulate_air / simulate_water /
simulate_light`, persisted in NVS, and toggled at runtime via `POST /sim`
without reflashing.

This replaces v3's compile-time `SIMULATION_MODE` define. One firmware image
covers every combination of "real" and "simulated" sensors. Use cases:

- **Develop the agent without sensors:** flip all three to sim, get
  realistic-looking data.
- **Quarantine a misbehaving sensor in production:** flip just the
  problematic one to sim until you can replace the hardware. Other sensors
  continue reading real values; the agent sees the `simulated` flag in the
  `/sensors` response and knows what to trust.
- **Fault-injection testing:** flip a sensor to sim to see how the agent
  reacts to fake-but-plausible data; flip back to real to verify recovery.

### Default Ranges (`simulation.cpp`)

| Sensor     | Field          | Simulated Range |
|------------|----------------|-----------------|
| DHT20      | air_temp       | 18.0 - 30.0 °C  |
| DHT20      | humidity       | 40 - 80 %       |
| DS18B20    | water_temp     | 20.0 - 28.0 °C  |
| LTR-553ALS | light          | 0 - 1000 lux    |

### Default state at boot

If NVS has no saved sim flags (fresh device, or after `nvs_flash_erase`),
all three default to **off** — real hardware reads. Toggle via `POST /sim`
to change.

### Agent honesty

Every `/sensors` response includes a `simulated` block that lists which
sensor was generating its current value via the random generator vs. real
hardware. The agent should ignore (or specially mark) simulated values when
making real decisions. See *HTTP Endpoints → GET /sensors*.

### Display indicator

Simulated sensors are shown in **yellow** with a `*SIM` (water) or `*`
(air, light) suffix on the device's status screen. Operator-visible at a
glance.

### Files
- `simulation.{cpp,h}` — random generators (`sim_air_temp`, `sim_humidity`,
  `sim_water_temp`, `sim_light`).
- `sim_state.{cpp,h}` — NVS load/save for the per-sensor flags.
- Sensor tasks (`dht20.cpp`, `ds18b20.cpp`, `light.cpp`) check the
  corresponding `g_state.simulate_*` atomic at the top of each iteration
  and dispatch accordingly.

---

## Thread Model

### 1. DHT20 Thread (Port A, I2C)
- **Interval:** Every 10 seconds
- **Reads:** Temperature (°C) + Humidity (%)
- **Writes:** `global.air_temp`, `global.humidity`, `global.seconds_since_boot_air`
- **Notes:** Port A shares the internal I2C bus with the touch controller
  (FT6336U), AXP2101 PMIC, BM8563 RTC, BMI270 IMU, and LTR-553ALS light sensor.
  All I2C transactions must be guarded by a `SemaphoreHandle_t` mutex. Keep
  transactions short and release the mutex immediately after each read.

### 2. DS18B20 Thread (Port B, GPIO 9, 1-Wire)
- **Interval:** Every 10 seconds
- **Reads:** Water temperature (°C)
- **Writes:** `global.water_temp`, `global.seconds_since_boot_water`
- **Hardware:** This build uses the M5Stack Grove DS18B20 unit, which integrates
  the 4.7kΩ pullup into the harness. No external pullup resistor is required.
  If a different DS18B20 module is ever substituted, verify the pullup is
  present — without it the bus reads as a stuck-high garbage value.
- **Notes:** The DS18B20 conversion takes up to 750ms. Use `requestTemperatures()`
  then `vTaskDelay(pdMS_TO_TICKS(800))` during the conversion window. The
  `vTaskDelay` yields to the FreeRTOS scheduler, which inherently feeds the
  task watchdog — no explicit watchdog feed needed. The 1-Wire bus is
  independent of I2C; no shared bus arbitration.

### 3. Light Sensor (Built-in LTR-553ALS, I2C)
- **Interval:** Every 30 seconds
- **Reads:** Ambient light in lux
- **Writes:** `global.light_lux`, `global.seconds_since_boot_light`
- **Notes:** Sits on the **internal** bus (`Wire1`) at address 0x23, alongside
  the touch controller, IMU, PMIC, and RTC. Driven via `M5.In_I2C` rather
  than raw `Wire1` so it serializes against `M5.update()`'s own internal-bus
  reads (touch + IMU). Does NOT need `g_wire_mutex` — `M5.In_I2C` has an
  internal mutex of its own. Read on its own thread interval; do not make it
  on-demand from the HTTP handler (avoids two code paths for the same sensor).

### 4. Camera (Initialized Once at Boot, Capture On Demand)

- **Triggered:** By HTTP request to `/snapshot` via synchronous `WebServer.h`,
  and continuously during the QR-code WiFi setup mode (`wifi_setup.cpp`).
- **Reads:** RGB565 frames from GC0308 (0.3 MP, native VGA 640×480). For
  `/snapshot` we capture VGA RGB565 and convert to JPEG via
  `frame2jpg(quality=80)` at request time — the GC0308 has no hardware JPEG.
- **Reinit:** Only on capture failure, never per-request. Currently we
  return HTTP 503 on `esp_camera_fb_get()` returning null without trying a
  reinit — adequate in practice; we haven't observed persistent failures.

**Pin map.** From `m5stack/M5CoreS3` → `src/utility/GC0308.cpp` (NOT from
the generic M5Stack arduino-esp32 docs, which list a different and
incorrect pin map for this device):

```
pin_pwdn = -1, pin_reset = -1, pin_xclk = -1,
pin_sccb_sda = 12, pin_sccb_scl = 11,
pin_d7 = 47, pin_d6 = 48, pin_d5 = 16, pin_d4 = 15,
pin_d3 = 42, pin_d2 = 41, pin_d1 = 40, pin_d0 = 39,
pin_vsync = 46, pin_href = 38, pin_pclk = 45,
xclk_freq_hz = 20000000, sccb_i2c_port = -1,
```

Critical: `pin_xclk = -1`. The GC0308 on the CoreS3 has its own clock; do
NOT drive XCLK from the ESP32. This is also why **GPIO 2 is free** (despite
v3's worry, the camera does not use GPIO 2 and Port A I2C is fine alongside
the camera).

**Init sequence.** Three things matter, in order:

1. `M5.In_I2C.release()` — hands the internal I2C bus from M5Unified to the
   camera driver for SCCB.
2. `esp_camera_init(&camera_config)` with `pixel_format = PIXFORMAT_RGB565`,
   `frame_size = FRAMESIZE_VGA`, `fb_count = 2`, `fb_location = CAMERA_FB_IN_PSRAM`,
   `grab_mode = CAMERA_GRAB_LATEST`. PSRAM-backed frame buffers are
   essential — VGA RGB565 is 614 KB per buffer; will not fit in DRAM.
3. After init, configure sensor tuning for the typical use case (looking at
   a backlit screen / QR code in mixed lighting):
   - `set_contrast(1)`, `set_brightness(0)`, `set_saturation(0)`
   - `set_ae_level(-1)` — bias auto-exposure toward darker scene-average
     so phone-screen-bright highlights don't clip
   - `set_gainceiling(GAINCEILING_2X)` — cap auto-gain to keep noise down
4. **First-frame discard:** call `esp_camera_fb_get()` + `fb_return()` once
   right after init. The GC0308's first integration window is unreliable.

**Bus coexistence after init.** This was the big v3-era investigation. The
key insight (from `m5stack/CoreS3-UserDemo` → `AppCameraModel.cpp`):

After `M5.In_I2C.release()`, **continue using `M5.In_I2C.readRegister/
writeRegister` directly** for other internal-bus peripherals (LTR-553,
PMIC, IMU, RTC). The M5Unified facade still routes through the bus
correctly, **provided you do NOT call `Wire1.end()/Wire1.begin()`** (that
breaks the bus state) and **provided I2C transactions are throttled to
≤100 kHz** rather than the default 400 kHz. The camera SCCB only re-enters
the bus when changing camera settings, which we don't do at runtime, so
the LTR-553 et al. have effectively unlimited bus time.

This is the pattern in `light.cpp`: every `M5.In_I2C` call passes
`100000L` as the frequency parameter. Same with battery-related reads
through `M5.Power`.

**Capture path.** Synchronous, not a dedicated task:

The HTTP `/snapshot` handler calls `camera_get_frame()`, which takes an
internal mutex, runs `esp_camera_fb_get()`, runs `frame2jpg()` to produce
a JPEG, and returns the JPEG buffer pointer. On success the handler
writes the JPEG to the socket and calls `camera_release_frame()` (frees
the JPEG buffer, returns the fb to the driver, releases the mutex). The
mutex serializes simultaneous `/snapshot` requests against each other.

The QR-setup mode in `wifi_setup.cpp` calls `esp_camera_fb_get()` directly
(bypassing the mutex, since no other task uses the camera during setup),
processes the VGA grayscale through quirc, and downsamples+mirrors the
RGB565 frame to 320×240 for live display preview.

Do NOT use `AsyncWebServer` for the camera endpoint — frame capture blocks.
Per-request init/deinit is unreliable on ESP32 (PSRAM leaks, DMA lockups).

### 5. WiFi + HTTP Server (Main Thread)
- **HTTP library:** `WebServer.h` (synchronous) — single-client device, no benefit
  from async for this use case.
- WiFi credentials come from NVS (`Preferences` namespace `wifi`, keys `ssid`
  and `pass`). On first boot or after a credential reset, the device enters
  QR-code setup mode (see *QR-Code WiFi Setup Mode* section below) which
  takes over the camera and display until the user shows a `WIFI:`-format QR.
- Listens on port 80.
- Routes: `/`, `/sensors`, `/status`, `/snapshot`, `POST /wifi/reset`.
- Never blocks on sensors — reads global state and returns immediately.

### 6. Display Update (Optional Main Thread Task)
- Interval: Every 5 seconds
- Shows: water temp, air temp, humidity, status icons
- This is low priority — if the display isn't needed, skip it entirely.
  The system works without display.

---

## Concurrency Model

### I2C Bus Topology on the CoreS3

The CoreS3 has **two independent I2C buses** — this is different from the
original M5Stack Core where Port A and the internal peripherals shared one
bus. Get the bus mapping wrong and you'll see `[Wire.cpp] NULL TX buffer
pointer` errors (talking to an uninitialized Wire instance) or NACKs at
addresses that should respond.

| Bus    | Pins (SCL/SDA) | Devices                                                    | Initialized by |
|--------|----------------|------------------------------------------------------------|----------------|
| `Wire1` | GPIO 11 / 12  | Touch (FT6336U), PMIC (AXP2101), RTC (BM8563), IMU (BMI270), AW9523 GPIO expander, **LTR-553ALS** | `M5.begin()`   |
| `Wire`  | GPIO 1 / 2    | Port A external Grove (DHT20, other external modules)      | **caller must call `Wire.begin(2, 1)`** |

### Wire Mutex

Tasks accessing the **internal bus** (`Wire1`) acquire `g_wire_mutex` before
calling `Wire1.beginTransmission()` and release it immediately after the
matching `endTransmission()` or final `read()`. This coordinates the LTR-553,
PMIC battery polling (when accessed via raw Wire1 rather than `M5.Power`), and
any future internal-bus consumers. M5Unified itself does its own locking on
its internal Wire1 access (touch/IMU reads from `M5.update()`), so we mostly
guard against contention between our own tasks.

Tasks accessing **Port A** (`Wire`) similarly serialize against each other,
but at present only the DHT20 lives there. If a second Port A device is ever
added (other Grove sensor, I2C OLED, etc.), introduce a separate
`g_port_a_mutex` for that bus — don't share the internal-bus mutex.

The PMIC battery level read goes through `M5.Power.getBatteryLevel()`, which
uses M5Unified's internally-locked I2C facade — no mutex acquisition needed
on our side for that call.

### Cross-Thread State: Atomic Fields

The ESP32-S3 is dual-core. FreeRTOS tasks may run on either core unless
explicitly pinned. `volatile` is insufficient for cross-core visibility.

Use `std::atomic<float>` for numeric fields and `std::atomic<uint32_t>` for
timestamps. Arduino-style `portMUX_TYPE` critical sections are an acceptable
alternative.

### Value + Timestamp Coherence

A reading (value) and its timestamp (`ts`) are two separate writes. In principle
a reader could observe a fresh value with a stale timestamp, or vice versa. In
practice, on the ESP32-S3 every `std::atomic<float>` and `std::atomic<uint32_t>`
load and store is a single hardware-atomic 32-bit word access — there is no
tearing of an individual field. The only inconsistency a reader can ever see
is a value from time `N` paired with a timestamp from time `N + a few µs`. The
agent's polling cadence is hundreds of milliseconds, so this window is invisible
to the system.

**No fences, no seqlock.** Single-writer atomics are sufficient.

If a future reading ever spans more than 32 bits as a unit (e.g. a struct of
multiple floats that must move atomically together), introduce a real sequence
counter at that point — writer increments seq to odd ("in progress"), writes
fields, increments seq to even ("done"); reader reads seq before/after and
retries on mismatch. Don't use the timestamp itself as the sequence number.

### Task Ownership

| Field              | Writer Task  |
|--------------------|--------------|
| air_temp, humidity | DHT20 thread |
| seconds_since_boot_air | DHT20 thread |
| water_temp         | DS18B20 thread |
| seconds_since_boot_water | DS18B20 thread |
| light_lux          | Light sensor thread |
| seconds_since_boot_light | Light sensor thread |
| wifi_rssi          | WiFi reconnect task |
| battery_pct        | Dedicated battery task (M5.Power facade self-locks) |
| wifi_connected     | WiFi reconnect task |
| boot_time_epoch    | NTP sync task (write-once) |

## Timestamp Scheme

Instead of emitting Unix epoch timestamps per-reading, the firmware uses a
two-field scheme that handles the NTP-never-succeeded case cleanly:

- **`seconds_since_boot`** — always meaningful, emitted per reading
- **`boot_epoch`** — Unix epoch of the most recent boot (from NTP), emitted
  once at the top level, set to `null` if NTP has never succeeded

The agent reconstructs absolute time as:
```
absolute_time = boot_epoch + seconds_since_boot
```

When `boot_epoch` is null, the agent uses its own poll timestamp as the
reference — no ambiguity about who's authoritative.

The state fields (`seconds_since_boot_*` and `boot_time_epoch`) are declared
in the `SensorState` struct in the next section.

## Global State

```cpp
#include <atomic>

struct SensorState {
  // Air
  std::atomic<float> air_temp{NAN};
  std::atomic<float> humidity{NAN};
  std::atomic<uint32_t> seconds_since_boot_air{0};

  // Water
  std::atomic<float> water_temp{NAN};
  std::atomic<uint32_t> seconds_since_boot_water{0};

  // Light
  std::atomic<uint16_t> light_lux{0};
  std::atomic<uint32_t> seconds_since_boot_light{0};

  // System
  uint32_t boot_time_epoch = 0;   // set once by NTP sync, 0 if unsynced
  std::atomic<int> wifi_rssi{0};
  std::atomic<int> battery_pct{0};

  // WiFi State
  std::atomic<bool> wifi_connected{false};
};
```

## HTTP Endpoints

### `GET /sensors`
Returns the latest readings. Timestamps are `seconds_since_boot` — always
meaningful regardless of NTP state. Content-Type: `application/json`.

```json
{
  "water_temp": 22.5,
  "air_temp": 24.1,
  "humidity": 55,
  "light": 320,
  "rssi": -45,
  "ss_boot_water": 8492,
  "ss_boot_air": 8492,
  "ss_boot_light": 8480,
  "wifi_ok": true,
  "simulated": {
    "air":   false,
    "water": false,
    "light": false
  }
}
```

- If a sensor hasn't reported in > 120 seconds, set its value to `null`
- If `isnan(value)`, emit `null` (NaN cannot be JSON-encoded)
- If WiFi is down, set `wifi_ok: false`
- The `simulated` block reports which sensor was reading from the random
  generator vs. real hardware at the time the response was built. Toggle
  via `POST /sim` (see Admin endpoints below). Agents should treat
  `simulated[<sensor>] == true` values as not-for-real-decisions data.

### `GET /status`
Returns system health and diagnostic state. `boot_epoch` is set to the
NTP-synced Unix boot time if available, or `null` if NTP has never succeeded.
Content-Type: `application/json`.

```json
{
  "uptime": 8495,
  "rssi": -45,
  "battery_pct": 85,
  "heap_free": 162000,
  "boot_epoch": 1714838400,
  "ntp_synced": true,
  "wifi_ok": true,
  "fw_version": "0.2.0",
  "camera_ok": true,
  "camera_err": 0,
  "psram_total": 8388608,
  "psram_free": 8056436,
  "psram_alloc_ok": true,
  "battery_mv": 794,
  "is_charging": false
}
```

Fields:

- **System** — `uptime` (seconds since boot), `heap_free` (bytes of internal
  free heap), `boot_epoch` (Unix epoch of boot, `null` if NTP never synced),
  `ntp_synced` (bool), `fw_version` (FW_VERSION constant).
- **Network** — `wifi_ok` (bool), `rssi` (dBm).
- **Camera** — `camera_ok` (true if `esp_camera_init` succeeded),
  `camera_err` (0 or last `esp_err_t`).
- **PSRAM** — `psram_total` and `psram_free` (bytes; both report 0 if PSRAM
  is not initialized as heap). `psram_alloc_ok` is a runtime probe — it
  attempts a 1 KB `ps_malloc` and reports the result, since on some
  toolchain configurations PSRAM is mapped but not heap-allocatable.
- **Battery / PMIC** — `battery_pct` (0–100 from `M5.Power.getBatteryLevel()`),
  `battery_mv` (raw voltage from AXP2101), `is_charging` (bool). On a unit
  with no LiPo connected (or a dead one the AXP2101 has cut out), expect
  `battery_pct: 0` and `battery_mv` near 0–800 mV. Healthy LiPo: 3000–4200 mV.

### `GET /snapshot`
Returns a JPEG image. No JSON wrapper — raw image data.
Content-Type: `image/jpeg`. Content-Length header set via
`s_server.setContentLength(len)` before `send()`.

If the camera fails (init failed at boot, or `esp_camera_fb_get()` returns
null), return HTTP 503 with body `camera unavailable`. Per-request reinit
is not currently attempted; a single reinit on persistent failure is a
documented future enhancement.

### `GET /`
Returns a small interactive HTML control panel intended for non-developer
users who want to inspect the device or toggle sim flags from a browser:

- **Live data** section — clickable links to `/sensors`, `/status`,
  `/snapshot`.
- **Sensor simulation** section — three CSS toggle switches (one per
  sensor: air, water, light) showing the current state ("real hardware"
  vs. "SIMULATED"). On page load the page calls `GET /sim` to populate
  toggles; toggling a switch issues `POST /sim?<id>=on|off` and re-renders
  from the response. A small feedback line reports success or errors.
- **Device name** section — shows the current mDNS hostname with a
  clickable URL, a text input + Save button, and a Reset to default link.
  Save POSTs to `/hostname?name=<value>`; Reset POSTs `/hostname?name=`.
  The page lazy-loads state via `GET /hostname` and notes that other
  devices' DNS caches may take ~2 minutes to forget the old name.
- **Admin** section — a red "Reset Wi-Fi credentials" button that posts to
  `/wifi/reset` after a `confirm()` dialog.

The page is served as a single self-contained HTML response (no external
CSS/JS); all styling and behavior are inlined. Agents should hit the JSON
endpoints directly rather than scraping this page.

### `POST /wifi/reset`
Wipes the saved WiFi credentials from NVS (`wifi` namespace) and reboots
the device, which then re-enters QR-code setup mode. Returns 200 with a
plain-text body before triggering the reboot. Other methods return 405.

### `GET /hostname`
Returns the current mDNS hostname plus the per-MAC default and a derived
URL. Useful for the home page UI and for agents that want to print a
direct link. Content-Type: `application/json`.

```json
{
  "current": "greenhouse-1",
  "default": "cores3-hydro-a1b2",
  "mac":     "a1b2c3d4e5f6",
  "url":     "http://greenhouse-1.local/"
}
```

`current` is whatever `device_hostname()` returns at request time — a
user-set override if present, otherwise the per-MAC default.

### `POST /hostname?name=<label>`
Sets the mDNS hostname. The new value is normalized (`trim()` +
`toLowerCase()`) and validated:

- lowercase letters `a`–`z`, digits `0`–`9`, hyphen (`-`) only
- 1 to 63 characters
- no leading or trailing hyphen

Pass an empty value (`POST /hostname?name=`) to clear the override and
revert to the per-MAC default. On success, the new name is persisted to
NVS (`device` namespace, key `host`), mDNS is re-announced live, and the
WiFi/OTA hostnames are updated (DHCP-side hostname fully refreshes on
next reconnect). Returns 200 with the same JSON shape as `GET /hostname`.

On invalid input, returns 400 with a short text body explaining the
error. On NVS write failure, returns 500.

Caveat: other devices' mDNS caches typically hold the old name for up to
~120 seconds after a rename, so both names may resolve briefly.

### `GET /sim`
Returns the current per-sensor simulation override flags as JSON.
Content-Type: `application/json`.

```json
{
  "air":   false,
  "water": false,
  "light": false
}
```

### `POST /sim?<flag>=<value>[&<flag>=<value>...]`
Sets one or more sim override flags. Recognized flags: `air`, `water`,
`light`. Recognized values (case-insensitive): `on`/`true`/`1`/`sim` to
enable simulation; `off`/`false`/`0`/`auto` to disable (real hardware).

Example:
```
POST /sim?air=on&water=auto
```

Returns 200 with the resulting state as JSON (same shape as `GET /sim`),
or 400 with a plain-text error if a value couldn't be parsed. Each
successful flag change is persisted to NVS (`sim` namespace) before the
response is sent — surviving reboots and reflashes.

### Auth note for admin endpoints
No authentication on any endpoint — the device is LAN-trusted. If exposed
to a hostile network, add per-deployment auth in front of these.

## Display (Optional)

If the TFT is active, cycle through a simple status screen:

```
WATER  22.5°C  OK
AIR    24.1°C  55%
LIGHT   320 lx
WiFi   -45 dBm  OK
BAT     85%
```

- Use ASCII labels (`OK`, `WARN`, `CRIT`) instead of emoji — M5GFX default font
  lacks emoji glyphs
- Small bitmap icons are an acceptable alternative (pixel-perfect, tiny flash cost)
- Color the water temp red if > 26°C, cyan if normal
- The display is a glanceable dashboard, not a control panel
- **Double-buffered drawing:** Render each frame into an off-screen `M5Canvas`
  sprite (allocated in PSRAM) and `pushSprite` to the live display in one
  blit. Drawing directly to the live display causes a visible flicker between
  the screen-clear and the redraw — the canvas pattern eliminates it entirely
  for ~150 KB of PSRAM cost.

## Edge Cases

### Sensor Freeze
If a sensor thread hasn't updated its reading in > 120 seconds, the HTTP
handler reports that value as `null` (also covers NaN). The agent detects
consecutive nulls and flags the sensor as possibly failed.

### Power Loss
- RTC (BM8563 with backup battery) retains time across short outages
- On boot: start sensor threads, init WiFi, init camera once, start HTTP server
- **Time sync:** After WiFi connects, perform NTP sync to set `boot_epoch` and
  update the RTC. If NTP never succeeds, `boot_epoch` stays null and the agent
  uses its own polling timestamps as the reference. `seconds_since_boot` values
  are always valid regardless of NTP state.
- No state to restore — it's a stateless box
- **Boot time:** ~8-12 seconds to first HTTP response (WiFi association + DHCP
  is the bottleneck, not sensor init)

### Camera Failure
- On capture failure: attempt a single reinit. If it fails again, HTTP 503.
  Log a reset-worthy error.

### WiFi Disconnection
If WiFi drops, sensor threads continue humming — readings buffer up in global
state. The HTTP server becomes unreachable until WiFi reconnects. Implement
exponential backoff reconnection:

| Attempt | Wait | Cumulative Time |
|---------|------|----------------|
| 1       | 15s  | 15s            |
| 2       | 30s  | 45s            |
| 3       | 60s  | 1m 45s         |
| 4       | 120s | 3m 45s         |
| 5       | 240s | 7m 45s         |
| 6+      | 300s | 12m 45s (cap)  |

Set `wifi_connected` to false so the display can show a disconnection indicator.
On reconnect, the agent gets a burst of fresh data — nothing is lost.

### Network Service Re-init on Reconnect

After each successful WiFi reconnect (not just the first connect), re-issue:

- `MDNS.end()` then `MDNS.begin(device_hostname())` — the mDNS responder
  must be fully restarted; the prior registration is invalid against the
  new IP/socket. Without this step the device's `.local` name silently
  stops resolving after the first WiFi drop.
- `ArduinoOTA.begin()` — OTA discovery rides on mDNS, same restart story.
- `WebServer` is fine; its listening socket survives a WiFi drop on ESP32.

Wire these up in an `onWiFiReconnect()` callback hooked to the
`SYSTEM_EVENT_STA_GOT_IP` event so they re-fire automatically on every
reconnect, including the first.

## QR-Code WiFi Setup Mode

Replaces the v3 WiFiManager + captive-portal approach. WiFiManager works,
but its captive-portal HTML form has known issues with the password field
on iOS / Android captive-portal mini-browsers, and steering users to "open
a real browser at 192.168.4.1" was friction we wanted to remove.

The CoreS3 has a working camera, so the cleaner pattern is to scan a
WiFi-format QR code. Standard format (Wi-Fi Alliance / ZXing):

```
WIFI:T:WPA;S:NetworkName;P:Password;;
```

iOS' "Share Wi-Fi" feature, Android's "Share network", and websites like
https://qifi.org all generate this format.

**Flow** (`wifi_setup.cpp::wifi_setup_run`):

1. On boot, `net_begin()` reads NVS for stored creds (`Preferences`
   namespace `wifi`, keys `ssid` + `pass`). If absent, it calls
   `wifi_setup_run()` which takes over the display and camera.
2. Brief splash screen with the one critical hint: **show a printed QR or
   a laptop monitor — phone screens cause moiré aliasing against the
   GC0308's Bayer pattern that destroys decode reliability**.
3. Camera warmup: discard ~30 frames over ~1.5 seconds so auto-exposure
   and gain converge before scanning starts.
4. Scan loop, ~7-10 fps:
   - Capture VGA RGB565 frame from the camera.
   - Convert to grayscale into quirc's image buffer (PSRAM-backed; the
     vendored quirc was patched to use `heap_caps_calloc` with
     `MALLOC_CAP_SPIRAM` — 307 KB grayscale buffer can't fit in DRAM).
   - Downsample+horizontal-mirror the same frame to 320×240 RGB565 in a
     PSRAM scratch buffer for the live display preview. Mirror is for the
     selfie-camera-style hand-eye coordination during aiming; the
     **un-mirrored** grayscale goes to quirc.
   - Run `quirc_end()` and check `quirc_count()`. On detect, run
     `quirc_decode()`. On success, parse the `WIFI:` payload.
5. On valid `WIFI:` parse: save to NVS via `wifi_creds_save()`, flash a
   green confirmation screen, return to `net_begin()` which connects with
   the new creds.
6. Abort: long-press the bottom-right 60×60 zone for ≥2 s.

**Re-entering setup on a configured device:**

- **Touch gesture at boot:** the `check_credential_reset()` path in
  `cores3-hydro.ino` watches for a long-touch in the top-right 60×60 zone
  during the first 3 seconds of boot. On detect it calls
  `net_reset_credentials()` which wipes NVS and reboots, falling through
  into setup mode on the next boot.
- **HTTP:** `POST /wifi/reset` from any LAN-connected client wipes NVS
  and reboots, with the same effect. Useful for remote re-setup without
  physical access.

**Implementation files:**

- `wifi_setup.{cpp,h}` — the scan loop, `WIFI:` parser, and
  `wifi_creds_load/save/clear` NVS helpers.
- `quirc.{c,h}`, `quirc_internal.h`, `decode.c`, `identify.c`,
  `version_db.c` — vendored from `dlbeer/quirc` upstream; `quirc.c` has a
  small ESP32-specific patch routing the image buffer to PSRAM.

## Implementation Notes

- **Build system: arduino-cli with M5Stack's arduino-esp32 fork.** Not
  PlatformIO. We tried PIO (both stock `platform=espressif32` and the
  `pioarduino` community fork) and neither would reliably initialize the
  CoreS3's Octal PSRAM; `ESP.getPsramSize()` reported 0 even with
  `memory_type=qio_opi` and `psram_type=opi` set. Since the M5Stack
  arduino-esp32 fork (`m5stack:esp32` in arduino-cli's package index) has
  the right CoreS3 board init out of the box and is the toolchain
  M5Stack's own examples are tested against, we migrated. Build and flash
  via the project's `build.ps1` wrapper (`-Env prod | sim | no-display`).
  Setup once via `setup.ps1` which installs arduino-cli, the M5Stack core,
  and the required libraries.
- **PSRAM:** CoreS3 has 8 MB Octal PSRAM. The M5Stack core configures it
  correctly out of the box once `m5stack:esp32:m5stack_cores3` is the
  selected board — no `platformio.ini` equivalents needed. Camera frame
  buffers go in PSRAM via the `esp_camera_init` config (`fb_location =
  CAMERA_FB_IN_PSRAM`); large response buffers (e.g. quirc image at VGA
  grayscale, 307 KB) should use `ps_malloc()` or vendored library patches
  routing through `heap_caps_*(..., MALLOC_CAP_SPIRAM)`. Keep the ~320 KB
  internal SRAM for tasks, stacks, and atomics — display canvas at 320×240
  16 bpp is 153 KB and goes to PSRAM via `M5Canvas::setPsram(true)`.
- **Library list** (installed via arduino-cli; versions pin to whatever
  `arduino-cli lib install` resolves — no project-level pinning):
  - `m5stack/M5Unified` — display, touch, IMU, PMIC, speaker, mic
  - `m5stack/M5GFX` — pulled in by M5Unified, provides M5Canvas
  - `bblanchon/ArduinoJson` (v7) — JSON serialization
  - `milesburton/DallasTemperature` + `paulstoffregen/OneWire` —
    DS18B20 over 1-Wire on GPIO 9
  - DHT20 read inline in `dht20.cpp` — no library needed; protocol is
    simple enough to keep in 30 lines and saves a dependency
  - LTR-553 read inline in `light.cpp` via `M5.In_I2C` — no library needed
  - `quirc` vendored at the sketch root (5 .c files + 2 .h files), with a
    small ESP32 patch routing the image buffer to PSRAM
  - `espressif/esp32-camera` — bundled with the M5Stack arduino-esp32 core
  - `Preferences` — bundled, used for NVS-backed WiFi creds
- **HTTP library:** `WebServer.h` (synchronous) — not AsyncWebServer. Single
  client, no benefit from async.
- **JSON serialization:** ArduinoJson v7's stack-allocated `JsonDocument`
  sized for the response (~256 bytes for `/sensors` and `/status`). Avoid
  `String` concatenation entirely — it fragments the heap, and this device
  needs to run for months between reboots.
- **WiFi setup:** QR-code scanning via the camera, replacing v3's
  WiFiManager captive portal. See *QR-Code WiFi Setup Mode* section above
  for flow.
- **WiFi reconnection:** explicit exponential backoff with `wifi_connected`
  flag (built-in auto-reconnect disabled via `WiFi.setAutoReconnect(false)`).
  Bind the app HTTP server in `http_server_begin()` after `net_begin()`
  completes initial connection.
- **WiFi credential reset:** Two paths. (a) Long-touch in the top-right
  60×60 zone during the first 3 seconds of boot — wipes NVS via
  `wifi_creds_clear()` and reboots. (b) `POST /wifi/reset` over HTTP —
  same effect, useful for remote re-setup. The CoreS3 has no
  boot-pollable hardware button, so touch is the only physical option.
- **Loop task stack:** raised to 16 KB via `SET_LOOP_TASK_STACK_SIZE(16 * 1024)`
  at file scope in `cores3-hydro.ino`. Default 8 KB is too small for
  quirc's decode pipeline running during `setup()` on the loop task.
- **NTP:** Sync after WiFi connects on boot. Retry on 24h cycle.
- **RTC battery:** The BM8563 needs a CR1220 coin cell in the holder on the
  back of the CoreS3 to retain time across power outages. Without it, every
  cold boot starts from epoch zero until NTP completes.
- **mDNS:** Per-device hostname for discovery. Default is
  `cores3-hydro-<last4mac>.local` (e.g. `cores3-hydro-a1b2.local`); the
  user can override via the home page or `POST /hostname` and the override
  is persisted in NVS (`device` namespace, key `host`). The runtime name
  comes from `device_hostname()` in `device_name.h`. mDNS must be
  re-initialized on each WiFi reconnect (see Network Service Re-init on
  Reconnect, above) and on each rename (see `net_apply_hostname_change()`).
- **OTA:** Include basic ArduinoOTA for firmware updates without USB. Same
  reconnect-restart story as mDNS.
- **Memory:** Global state is tiny (~100 bytes with atomics). No heap
  allocation in sensor threads. JSON responses use stack-allocated
  `JsonDocument`. Camera frames live in PSRAM via the camera driver.
- **No authentication:** LAN-trusted device. No auth on HTTP endpoints.
- **Watchdog:** DS18B20's 750ms conversion blocks the sensor thread.
  `vTaskDelay(pdMS_TO_TICKS(800))` during the wait yields to the scheduler and
  inherently feeds the task watchdog. **Do not disable the watchdog** — a hung
  sensor thread should reboot the device.
- **Tunables in `config.h`:** Centralize compile-time constants:
  `SENSOR_STALE_S` (default 120), `DHT_INTERVAL_MS` (10000),
  `DS18B20_INTERVAL_MS` (10000), `LIGHT_INTERVAL_MS` (30000),
  `WIFI_RECONNECT_BACKOFF_*`, `CAMERA_TIMEOUT_MS` (3000), and pin assignments.
  One header to tune from.
- **Timestamp convention:** `seconds_since_boot` per reading (always meaningful)
  + `boot_epoch` at top level (`null` if NTP never succeeded). Agent reconstructs
  absolute time as `boot_epoch + seconds_since_boot`.
- **HTTP headers:** Set `Content-Type: application/json` on JSON endpoints and
  `Content-Type: image/jpeg` + `Content-Length` on `/snapshot`.
- **Sensor abstraction (future):** Define a common interface (`read() → float`)
  that each driver implements. Makes the pattern reusable.

## Files (arduino-cli flat sketch layout)

```
cores3-hydro/
├── cores3-hydro.ino        // setup() + FreeRTOS task creation (entry point;
│                           //   .ino name MUST match folder name)
├── config.h                // SENSOR_STALE_S, intervals, pin map, version,
│                           //   timezone, hardcoded-WiFi gesture geometry
├── sensors.h               // SensorState struct (std::atomic fields)
├── dht20.{cpp,h}           // DHT20 reader (Port A, I2C, inline driver)
├── ds18b20.{cpp,h}         // DS18B20 reader (Port B, GPIO 9, 1-Wire)
├── light.{cpp,h}           // LTR-553ALS reader (internal bus via M5.In_I2C)
├── battery.{cpp,h}         // M5.Power.getBatteryLevel poll task
├── camera.{cpp,h}          // GC0308 init + /snapshot capture path
├── wifi_setup.{cpp,h}      // QR-code WiFi setup, NVS creds, parser
├── http_server.{cpp,h}     // Sync WebServer + handlers
├── net.{cpp,h}             // WiFi connect, reconnect, mDNS, OTA, NTP
├── simulation.{cpp,h}      // random generators (sim_air_temp, etc.)
├── sim_state.{cpp,h}       // NVS persistence for per-sensor sim overrides
├── display.{cpp,h}         // M5Canvas-backed status screen
├── quirc.{c,h}             // QR decoder (vendored from dlbeer/quirc,
│                           //   small ESP32 patch for PSRAM image buffer)
├── quirc_internal.h        //   "
├── decode.c                //   "
├── identify.c              //   "
├── version_db.c            //   "
├── build.ps1               // arduino-cli wrapper: -Env, -Upload, -Monitor
├── setup.ps1               // one-time arduino-cli + library install
├── docs/
│   └── cores3-firmware-spec.md   // this file
└── .gitignore
```

The flat layout is required by arduino-cli — the `.ino` must be at the
sketch root and have the same name as the folder. All `.cpp/.h/.c` files
in the same directory are compiled into the sketch.

Build:
```powershell
.\build.ps1                      # compile prod (default)
.\build.ps1 -Env sim             # simulation env
.\build.ps1 -Env no-display      # no display task
.\build.ps1 -Upload -Monitor     # full cycle
```

Build flags per env are passed via `--build-property compiler.cpp.extra_flags`:

- `prod` — no extra flags (default; runtime sim overrides via `POST /sim`)
- `no-display` — `-DDISABLE_DISPLAY=1` (compiles out the display task —
  RAM-saving choice, not a runtime toggle)

(Using `compiler.cpp.extra_flags` rather than `build.extra_flags` is
important: the latter would *replace* the board manifest's
`ARDUINO_M5STACK_CORES3 / ARDUINO_USB_CDC_ON_BOOT / BOARD_HAS_PSRAM`
defaults, breaking the build. `compiler.cpp.extra_flags` appends.)

## Future: Action Endpoints

When the air pump moves from hardware timer to agent control, add:

### `POST /pump/on`
### `POST /pump/off`

These will set a GPIO to control a relay on one of the Grove ports. The firmware
enforces a hardware safety: if the agent hasn't sent a pump command in 3 hours,
the firmware cycles the pump on for 15 minutes as a fallback.

This dead-man timer is the **sole exception** to the "firmware never makes
decisions" rule. It is a hardware-safety mechanism, not a normal control path.
