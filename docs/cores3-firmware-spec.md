# CoreS3 Firmware Specification — Hydroponic Monitor (v14)

> **v14 changes:** Bug fix for the "Mounted upside down" feature, plus a
> small home-page polish. The v12 implementation relied on
> `set_hmirror(1) + set_vflip(1)` on the GC0308 to produce a 180° rotation;
> verified empirically on production hardware that the esp32-camera driver's
> `set_hmirror()` write is silently dropped (the sensor's `status.hmirror`
> field updates to reflect the request but the captured pixels don't
> change). `set_vflip()` works fine, but a V-flip alone leaves a horizontal
> mirror after the physical R180 mount, so `/snapshot` came out mirrored
> for users who actually mounted the unit upside-down. The fix is to do
> the rotation **in software** in `camera_get_frame()`: when
> `g_state.display_flipped` is set, the RGB565 buffer is reverse-walked
> in-place (pixels at index `i` and `len/2 - 1 - i` are swapped), which is
> exactly R180. Cost is ~15-30 ms per snapshot on the S3, acceptable for an
> on-demand endpoint. The sensor is now left at `hmirror=0, vflip=0`;
> `camera_set_flip()` is a documented no-op kept so the `/display` POST
> handler doesn't need to know how the flip is implemented. Home page also
> gains port labels in the sensor list (`DHT20 on Port A`, `DS18B20 on
> Port B`, `LTR-553ALS (internal)`) so users know which Grove connector
> hosts each sensor. `FW_VERSION` bumps to `0.7.1`.
>
> **v13 changes:** Two user-facing additions and one breaking change to the
> HTTP contract.
> **(1) Temperature units are user-selectable** (Celsius / Fahrenheit). A
> new `units.{cpp,h}` module owns an NVS-persisted preference (`units`
> namespace, key `temp`, uint8: 0 = Celsius, 1 = Fahrenheit), default
> Fahrenheit. Both the on-screen dashboard and the `/sensors` JSON emit
> temperatures in the selected unit — option B in the design discussion:
> the device converts on egress so the upstream agent (OpenClaw) doesn't
> have to. The atomics in `g_state` still hold raw Celsius (that's what
> the hardware drivers natively return); conversion happens only at the
> printf / serialize sites via `temp_in_user_unit(c)`. The `/sensors`
> response gains a new `temperature_units` string field
> (`"celsius"` | `"fahrenheit"`) so any client can disambiguate. New
> endpoint: `GET`/`POST /units?temperature_units=celsius|fahrenheit`.
> **(2) Sensors are now tristate** — REAL, SIMULATED, or DISABLED. The
> per-sensor `simulate_<x>` booleans in `SensorState` are replaced with
> `<x>_mode` atomics of type `std::atomic<uint8_t>` backing the
> `SensorMode` enum (REAL=0, SIMULATED=1, OFF=2; the C++ identifier is
> `OFF` only to dodge ESP32's `#define DISABLED` GPIO-mode macro, the
> wire-format label is still `"disabled"`). DISABLED is intended for "I
> intentionally do not have this sensor — and that's fine" (e.g.
> microgreens with no water tank). Sensor tasks short-circuit out of the
> bus access when their mode is DISABLED, leaving the value atomic and
> timestamp untouched; the `/sensors` handler emits `null` for the value
> regardless. NVS migration is transparent: old bool keys in the `sim`
> namespace (`false`=0=REAL, `true`=1=SIMULATED) decode correctly under
> `getUChar`.
> **(3) Breaking HTTP changes** for any client of `/sensors` and `/sim`.
> The `simulated` boolean object in `/sensors` is **replaced** by a
> `status` object whose values are the strings `"real"`, `"simulated"`,
> or `"disabled"` — strictly more expressive than the old booleans
> (disabled needs its own bucket, and `null` value + status disambiguates
> "missing because off" from "missing because stale/errored"). `/sim`'s
> POST grammar changes too: the values are now `real`, `sim` /
> `simulated`, `disabled` / `off` / `none` (case-insensitive). The
> pre-v0.7.0 boolean shorthand (`on`/`off`/`true`/`false`/`1`/`0`/`auto`)
> is no longer accepted because `off` would have been ambiguous between
> the old "disable simulation" and the new "turn the sensor off". `GET
> /sim` returns mode strings instead of booleans.
> Home-page UI: a new **Temperature units** section with a C/F dropdown,
> and the per-sensor toggles are replaced by mode dropdowns
> (Real / Simulated / Disabled). The display shows the selected unit's
> suffix (`F` or `C`) and renders `OFF` in dark grey for disabled sensors.
> `FW_VERSION` bumps to `0.7.0`.
>
> **v12 changes:** Add a user-toggleable "Mounted upside down" orientation
> flip that rotates both the display and the camera 180° together. The use
> case is mechanical: with the unit mounted normally, the Grove cable
> emerging from Port A (and any thick water-temp lead) exits the top of
> the chassis and droops down across the screen. Mounting the unit
> inverted routes the cables down behind, but then the display reads
> upside down and `/snapshot` images come out flipped. The new flag
> handles both halves at once: display rotation switches from `1` to `3`
> (landscape, 180°), and the GC0308 sensor gets `set_hmirror(1)` +
> `set_vflip(1)` so captured frames match the screen orientation. Touch
> coordinates rotate with the display, so the credential-reset top-right
> zone tracks correctly. Settings live in `g_state.display_flipped`,
> persisted via the existing `display` NVS namespace (new key `flip`,
> uint8 0/1), and exposed as a new `flipped` boolean on `GET`/`POST
> /display`. The home page's Display section gains an orientation toggle.
> Setup order changes slightly: `display_settings_load()` now runs right
> after `M5.begin()` so the very first `setRotation()` call uses the
> stored orientation — otherwise the boot screen and credential-reset
> prompt would briefly display in the wrong orientation. New
> `camera_set_flip(bool)` exposes the SCCB write at runtime; it holds the
> camera capture mutex so it can't race with an in-flight `/snapshot`.
> `FW_VERSION` bumps to `0.6.0`.
>
> **v11 changes:** Fix DS18B20 pin assignment. The spec and config had
> `PIN_DS18B20_DATA = 9`, but on the M5Stack CoreS3 DIN Base Port B's
> Grove pin 1 (yellow, the 1-Wire data line) actually routes to **GPIO
> 8**, not GPIO 9 — GPIO 9 is Grove pin 2 (the unused "white" pin for a
> 1-Wire device). The wrong-pin guess was latent because no real DS18B20
> had been wired up until now; like the DHT20 `Wire.begin` bug in v9,
> simulation mode masked the gap. Fix is the one-line change in
> `config.h`; the task logic in `ds18b20.cpp` is otherwise unchanged.
> Documentation updated across `CLAUDE.md`, `config.h`, `ds18b20.{cpp,h}`,
> and this spec to reflect GPIO 8. `FW_VERSION` bumps to `0.5.3`.
>
> **v10 changes:** Dashboard now shows temperatures in **Fahrenheit**.
> The on-screen `WATER` and `AIR` rows display `°F` instead of `°C`; a
> small `c_to_f()` helper in `display.cpp` converts at the printf site.
> The water-warm threshold (`wt > 26.0f` → red tint) still compares in
> Celsius — only the rendered string changes. **The HTTP API stays in
> Celsius:** `/sensors` continues to emit `water_temp` and `air_temp` in
> °C, the atomics in `g_state` are still °C, and NVS state is unchanged.
> This is intentional — the upstream agent (OpenClaw) reads the HTTP
> contract and expects metric units; the display change is purely
> cosmetic. `FW_VERSION` bumps to `0.5.2`.
>
> **v9 changes:** Fix DHT20 never produced a real reading. The Port A
> external Grove bus (`Wire`, SDA=GPIO 2, SCL=GPIO 1) was never initialized
> as master — `M5.begin()` only brings up the internal `Wire1`, so the
> DHT20 task's boot-time probe at 0x38 always NACKed and the task fell
> through to the "idle, re-probe every 5 min" path forever. The bug was
> latent because every prior bring-up ran with the air-temp simulation
> override on, which bypasses the bus entirely; it only surfaced once a
> real DHT20 unit was wired to Port A. Fix: call `Wire.begin(2, 1)` in
> `setup()` immediately before `dht20_start()`, as the I2C-bus-topology
> table has always required. No API or schema changes; no behavioral
> changes for the existing simulated path. `FW_VERSION` bumps to `0.5.1`.
>
> **v8 changes:** Reverted first-time WiFi setup from QR-code scanning back
> to **tzapu/WiFiManager** AP + captive portal. The QR path turned out to be
> too fragile in practice — moiré aliasing against the GC0308's Bayer
> pattern made phone-screen QRs nearly undecodable, and even
> printed/laptop-screen QRs needed careful staging. The new flow: on first
> boot (or after a credential reset) the device opens an open SoftAP named
> `cores3-hydro-setup-<last4mac>`; the user connects from a phone/laptop
> and is steered to a captive portal at http://192.168.4.1/ with a scanned
> SSID list and a password field. Credentials are stored by WiFiManager via
> the ESP32's native WiFi config — the project's old `wifi` Preferences
> namespace is no longer used (and is wiped on credential reset for
> migration cleanup). Portal timeout is 3 minutes; on timeout the device
> reboots and re-enters setup. The camera is **kept** because
> `GET /snapshot` still uses it, but it's no longer involved in setup.
> Quirc and the scan loop are deleted entirely (no fallback). Both
> credential-reset paths (top-right long-touch at boot, `POST /wifi/reset`)
> stay; the home page's admin button is relabeled "Reconfigure WiFi". New
> module structure: `wifi_setup.{cpp,h}` is now a thin WiFiManager wrapper;
> `quirc.{c,h}`, `quirc_internal.h`, `decode.c`, `identify.c`,
> `version_db.c` are removed. `FW_VERSION` bumps to `0.5.0`.
>
> **v7 changes:** Display gains user-configurable backlight brightness and
> idle-driven dim/sleep. The screen is otherwise on 24/7 with a fully
> static layout, which on an IPS LCD risks backlight degradation and
> mild image persistence over months. The display task now runs a
> three-state machine (ACTIVE → DIM → SLEEP) gated by time since last
> touch: at `dim_ms` of idle the backlight drops to ~25% of the user
> brightness; at `sleep_ms` the backlight goes fully off and the canvas
> blit is skipped. Either timeout set to `0` disables that transition.
> Touch wakes the display — `M5.Touch.getCount()` is polled in the main
> `loop()` and updates `g_state.last_touch_ms`. OTA-in-progress overrides
> sleep so the takeover screen is always visible. Settings live in
> `g_state.display_brightness / display_dim_ms / display_sleep_ms`,
> persisted via the new NVS namespace `display` (keys `bright`, `dim_ms`,
> `sleep_ms`) and exposed over `GET`/`POST /display`. Defaults: brightness
> 180/255, dim 30 s, sleep 5 min. New module: `display_settings.{cpp,h}`.
> Home page gains a Display section (slider + two number inputs).
> `FW_VERSION` bumps to `0.4.0`.
>
> **v6 changes:** Adds an HTTP-based OTA path alongside the existing
> ArduinoOTA push: `GET /ota` serves a tiny browser page with a file
> picker; `POST /ota/upload` accepts a multipart-form-data upload of a
> raw `.bin`, buffers the entire image in PSRAM via `ps_malloc()`, then
> writes it to flash via `Update.h` once fully received. Buffer-then-burn
> is safer than streaming straight to flash — a torn upload leaves the
> running partition untouched. The new `g_state.ota_in_progress` atomic
> flag idles all sensor tasks and switches the display to a takeover
> screen during the upload; `camera_stop()` is called at the start of the
> upload to free the camera framebuffers (~1.2 MB) so the upload buffer
> fits. ArduinoOTA stays in place for the build-host dev loop and gains a
> `build.ps1 -Network` switch that pushes via the network protocol. New
> module: `ota.{cpp,h}`. `FW_VERSION` bumps to `0.3.0`. No auth, no
> signature verification — same LAN-trust model as the rest of the device.
>
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
> portal with QR-code-based setup via the camera (later reverted in v8);
> rewrote build-system guidance from PlatformIO to arduino-cli with
> M5Stack's arduino-esp32 fork (PIO didn't reliably initialize OPI PSRAM
> on the CoreS3); added `GET /` landing page and `POST /wifi/reset`
> endpoint; `/status` now exposes camera, PSRAM, and battery diagnostics;
> corrected GPIO 2 / Port A note (pin_xclk = -1 means no conflict); files
> section reflects flat arduino-cli sketch layout with vendored quirc
> (also removed in v8); **simulation is now a per-sensor runtime override**
> (`GET`/`POST /sim`, NVS-persisted) instead of a compile-time flag —
> `/sensors` exposes a `simulated` block so the agent can tell real from
> fake; the `cores3-sim` build env is removed.
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

## Sensor Mode

Each sensor independently runs in one of three modes: **real** (default,
read hardware), **simulated** (random values from `simulation.cpp`), or
**disabled** (skip the read entirely; the value reads as `null` with
`status: "disabled"` on `/sensors`). The mode for each sensor is stored in
`g_state.air_mode / water_mode / light_mode` (atomic<uint8_t> backing the
`SensorMode` enum), persisted in NVS, and toggled at runtime via
`POST /sim` without reflashing.

This replaces v3's compile-time `SIMULATION_MODE` define. One firmware image
covers every combination of real / simulated / disabled sensors. Use cases:

- **Develop the agent without sensors:** flip all three to simulated, get
  realistic-looking data.
- **Quarantine a misbehaving sensor in production:** flip just the
  problematic one to simulated until you can replace the hardware. Other
  sensors continue reading real values; the agent sees `status` in the
  `/sensors` response and knows what to trust.
- **Fault-injection testing:** flip a sensor to simulated to see how the
  agent reacts to fake-but-plausible data; flip back to real to verify
  recovery.
- **Intentionally absent sensor:** flip to disabled when a deployment
  doesn't physically include the sensor (e.g. microgreens with no water
  tank → water disabled). The agent receives `null` + `status: "disabled"`
  and treats the signal as not-present rather than missing-due-to-error.

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

### 2. DS18B20 Thread (Port B, GPIO 8, 1-Wire)
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

- **Triggered:** By HTTP request to `/snapshot` via synchronous `WebServer.h`.
  (Was also used continuously during v7's QR-code WiFi setup mode; that
  path was removed in v8.)
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
3. After init, configure sensor tuning for the typical use case (a
   plant-tank scene in mixed indoor lighting; settings were originally
   chosen for QR scanning but remain reasonable defaults for `/snapshot`):
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

As of v8, the camera is no longer used during WiFi setup — `/snapshot` is
the sole consumer.

Do NOT use `AsyncWebServer` for the camera endpoint — frame capture blocks.
Per-request init/deinit is unreliable on ESP32 (PSRAM leaks, DMA lockups).

### 5. WiFi + HTTP Server (Main Thread)
- **HTTP library:** `WebServer.h` (synchronous) — single-client device, no benefit
  from async for this use case.
- WiFi credentials are owned by WiFiManager / the arduino-esp32 WiFi stack
  (native ESP32 WiFi config storage). On first boot or after a credential
  reset, the device enters WiFiManager's captive-portal AP mode (see
  *WiFi Setup Mode (WiFiManager)* section below) which serves an SSID
  scan + password form at http://192.168.4.1/ until the user finishes
  or the 3-minute timeout fires.
- Listens on port 80.
- Routes: `/`, `/sensors`, `/status`, `/snapshot`, `/sim`, `/hostname`, `/display`,
  `POST /wifi/reset`, `/ota`, `POST /ota/upload`.
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
  "water_temp": 72.5,
  "air_temp": 75.4,
  "humidity": 55,
  "light": 320,
  "temperature_units": "fahrenheit",
  "rssi": -45,
  "ss_boot_water": 8492,
  "ss_boot_air": 8492,
  "ss_boot_light": 8480,
  "wifi_ok": true,
  "status": {
    "air":   "real",
    "water": "disabled",
    "light": "real"
  }
}
```

- `temperature_units` is the user-selected unit (`"celsius"` or
  `"fahrenheit"`); `water_temp` and `air_temp` are emitted in that unit.
  Toggle via `POST /units`. The atomics in `g_state` are still raw °C —
  conversion happens at serialize time in `temp_in_user_unit`.
- If a sensor's mode is `"disabled"`, the corresponding value field is
  `null` regardless of any stored reading — the user explicitly opted out.
- If a sensor hasn't reported in > 120 seconds, its value is `null` too
  (stale). The `status` field still reflects the mode (`"real"` or
  `"simulated"`), which is how an agent disambiguates "stale/erroring" from
  "intentionally off".
- If `isnan(value)`, emit `null` (NaN cannot be JSON-encoded).
- If WiFi is down, set `wifi_ok: false`.
- The `status` block reports each sensor's mode: `"real"` (hardware read),
  `"simulated"` (`sim_*()` generator), or `"disabled"` (sensor intentionally
  off). Toggle via `POST /sim` (see Admin endpoints below). Agents should
  treat `"simulated"` values as not-for-real-decisions data, and treat
  `"disabled"` as "this signal does not exist in this deployment."

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
- **Temperature units** section — a dropdown (Fahrenheit / Celsius). On
  load the page calls `GET /units` and selects the current value; changing
  the dropdown issues `POST /units?temperature_units=<v>` and reflects the
  resulting state.
- **Sensor mode** section — three dropdowns (one per sensor: air, water,
  light) with options `real hardware`, `SIMULATED`, `disabled`. On load
  the page calls `GET /sim` to populate the dropdowns; changing a value
  issues `POST /sim?<id>=real|simulated|disabled` and re-renders from the
  response. A small feedback line reports success or errors.
- **Device name** section — shows the current mDNS hostname with a
  clickable URL, a text input + Save button, and a Reset to default link.
  Save POSTs to `/hostname?name=<value>`; Reset POSTs `/hostname?name=`.
  The page lazy-loads state via `GET /hostname` and notes that other
  devices' DNS caches may take ~2 minutes to forget the old name.
- **Admin** section — a "Firmware update" link that opens `GET /ota`,
  plus a red "Reset Wi-Fi credentials" button that posts to
  `/wifi/reset` after a `confirm()` dialog.

The page is served as a single self-contained HTML response (no external
CSS/JS); all styling and behavior are inlined. Agents should hit the JSON
endpoints directly rather than scraping this page.

### `POST /wifi/reset`
Wipes the saved WiFi credentials (via `WiFiManager::resetSettings()` plus
`WiFi.disconnect(true, true)`; the legacy `wifi` Preferences namespace is
also cleared for migration cleanup) and reboots the device, which then
re-enters WiFiManager's captive-portal AP mode. Returns 200 with a
plain-text body that includes the AP name (`cores3-hydro-setup-<mac>`)
and the portal URL before triggering the reboot. Other methods return 405.

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
Returns the current per-sensor mode as JSON. Content-Type:
`application/json`. Each value is one of `"real"`, `"simulated"`, or
`"disabled"`.

```json
{
  "air":   "real",
  "water": "disabled",
  "light": "real"
}
```

### `POST /sim?<sensor>=<mode>[&<sensor>=<mode>...]`
Sets one or more sensor modes. Recognized sensors: `air`, `water`,
`light`. Recognized modes (case-insensitive):

- `real` → REAL — hardware read
- `sim` or `simulated` → SIMULATED — value from `sim_*()` generator
- `disabled`, `off`, or `none` → DISABLED — value reads as `null` on
  `/sensors` and `status` is `"disabled"`

Example:
```
POST /sim?air=real&water=disabled&light=simulated
```

Returns 200 with the resulting state as JSON (same shape as `GET /sim`),
or 400 with a plain-text error if a value couldn't be parsed. Each
successful change is persisted to NVS (`sim` namespace, uint8 per sensor)
before the response is sent — surviving reboots and reflashes. Old
pre-v0.7.0 boolean keys in this namespace decode correctly under
`getUChar` (false=REAL, true=SIMULATED), so existing installs migrate
silently. The pre-v0.7.0 boolean shorthand (`on`/`off`/`true`/`false`/
`1`/`0`/`auto`) is **not** accepted by this endpoint — `off` would have
been ambiguous between the old "disable simulation" and the new
"disable the sensor entirely."

### `GET /units`
Returns the current temperature-unit preference as JSON. Content-Type:
`application/json`.

```json
{
  "temperature_units": "fahrenheit"
}
```

The same field appears in every `/sensors` response, so a client that
already reads `/sensors` does not need a separate call here — this
endpoint exists for symmetry with `/sim`, `/hostname`, and `/display`.

### `POST /units?temperature_units=<unit>`
Sets the temperature unit. Recognized values (case-insensitive):
`celsius` or `c`; `fahrenheit` or `f`. Persisted to NVS (`units`
namespace, key `temp`, uint8: 0=Celsius, 1=Fahrenheit). The change is
reflected in `GET /sensors` and on the device dashboard immediately.
Returns 200 with the resulting state as JSON (same shape as
`GET /units`), or 400 on bad input.

### `GET /display`
Returns the current display power settings as JSON. Content-Type:
`application/json`.

```json
{
  "brightness": 180,
  "dim_ms":     30000,
  "sleep_ms":   300000,
  "flipped":    false
}
```

- `brightness` — backlight intensity when ACTIVE, 0..255.
- `dim_ms` — milliseconds of touch idle before the screen dims to ~25%
  of `brightness`. `0` disables the dim transition.
- `sleep_ms` — milliseconds of touch idle before the backlight goes
  fully off and the canvas blit is skipped. `0` disables sleep.
- `flipped` — `true` when the unit is mounted upside down. Display
  rotates 180° (rotation `3` instead of `1`) and `/snapshot` frames are
  rotated 180° in software (in-place RGB565 reverse) before JPEG
  encoding, so captured images match the screen orientation.

### `POST /display?<field>=<value>[&<field>=<value>...]`
Updates one or more display settings. All four fields are optional;
only listed fields are changed. Validation:

- `brightness`: integer 0..255
- `dim_ms`, `sleep_ms`: integer 0..86400000 (24 h)
- `flipped`: `on`/`off`/`true`/`false`/`1`/`0`

On success: each changed field is persisted to NVS (`display` namespace,
keys `bright`, `dim_ms`, `sleep_ms`, `flip`); if `flipped` changed the
display rotation is reapplied immediately (subsequent `/snapshot` calls
pick up the new orientation flag the next time a frame is fetched). The
wake timestamp is reset (any POST is treated as user activity), and 200
is returned with the same JSON shape as `GET /display`. On invalid
input, returns 400 with a short text body. Other methods return 405.

OTA-in-progress overrides sleep — the takeover screen is always shown at
`brightness` regardless of idle state.

### `GET /ota`
Returns a small self-contained HTML page with a file picker, a submit
button, an upload-progress bar, and an inline JavaScript that POSTs the
selected `.bin` to `/ota/upload` via `XMLHttpRequest` (so we can drive
the `<progress>` element from `xhr.upload.onprogress`). The page also
shows the current `FW_VERSION` for sanity-checking before flashing.

This page exists for browser-driven updates from a phone or laptop;
agents should use the underlying `POST /ota/upload` endpoint directly.

### `POST /ota/upload`
Accepts a multipart-form-data upload of a raw firmware image (the same
`.bin` produced by `arduino-cli compile`). The form field name is
`firmware`; only one file per request is honored.

**Architecture: buffer-then-burn.** The full image is staged in PSRAM via
`ps_malloc()` and only written to flash once the upload has completed
successfully. This is safer than streaming straight to flash — if the
upload is torn (network failure, client cancel, declared size mismatch),
the running partition is untouched and the device continues normally
after the failure response.

**Upload state machine** (`ota.cpp`):

1. `UPLOAD_FILE_START` — read `Content-Length`, sanity-cap at 6 MB, set
   `g_state.ota_in_progress = true`, call `camera_stop()` to release the
   camera framebuffers (~1.2 MB of PSRAM), `ps_malloc(content_length)`,
   prep the `Update` library.
2. `UPLOAD_FILE_WRITE` — `memcpy` each chunk into the PSRAM buffer at
   the running offset; reject if the cumulative size exceeds the
   declared `Content-Length`.
3. `UPLOAD_FILE_END` — `Update.begin(received) → Update.write(buf,
   received) → Update.end(true)`. The library handles partition swap
   and CRC validation. Free the buffer.
4. Completion handler — `send(200, text/html, "Update OK — rebooting")`
   then `delay(2000); ESP.restart()`.

`Content-Length` is opted in via `server.collectHeaders()` so the chunk
handler can read it at `UPLOAD_FILE_START`. The allocation includes the
multipart envelope overhead (a few hundred bytes); we track the actual
firmware byte count separately and only `Update.write()` that count.

**Error responses:** any failure (missing/bogus Content-Length, oversize,
`ps_malloc` returns null, size mismatch, `Update.*` failure, client
abort) records an error string. The completion handler returns
`500 application/json` with body `{"ok": false, "error": "<message>"}`,
clears `ota_in_progress`, and leaves the running partition untouched.

**Side effects during upload:**

- All sensor tasks (DHT20, DS18B20, LTR-553, battery) check
  `g_state.ota_in_progress` at the top of their loops and short-circuit
  to a `vTaskDelay` — no hardware reads, no atomic writes.
- The display switches from the dashboard to a navy "OTA UPDATE" screen
  noting that sensors are paused and the device will reboot when flash
  is written.
- The camera is fully de-initialized via `camera_stop()` (calls
  `esp_camera_deinit()`); `/snapshot` will return 503 from this point
  until the next boot. Acceptable — the device is rebooting in seconds.

**Notes / caveats:**

- No CRC/SHA verification beyond what the `Update` library does
  internally (CRC over the partition header). Signature verification is
  a future enhancement.
- No authentication. Same LAN-trust model as the rest of the API.
- The 6 MB cap is below the typical ESP32 OTA partition size (~6.25 MB
  for the default 16 MB-flash layout). Revisit if we customize
  partitions.
- If the running firmware was itself flashed via this path and an
  earlier upload had already partial-staged a buffer, the
  `ota_record_error` cleanup (which calls `Update.abort()`) makes the
  next attempt safe.

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

## WiFi Setup Mode (WiFiManager)

Reverts v4's QR-code experiment back to tzapu/WiFiManager. The QR path
was conceptually appealing — point camera at QR, done — but in practice
moiré aliasing between phone-screen subpixel matrices and the GC0308's
Bayer grid made decode reliability poor, and even printed/laptop-screen
QRs needed careful staging. WiFiManager's captive-portal form is the
boring, well-trodden path; we accept the iOS/Android captive-portal
quirks that originally motivated the QR detour as a worthwhile tradeoff
for setup that actually works.

**Flow** (`wifi_setup.cpp::wifi_setup_run`):

1. On boot, `net_begin()` calls `wifi_setup_run()` unconditionally.
2. Inside, a `WiFiManager` instance is configured with a 3-minute
   `setConfigPortalTimeout(180)`, `setBreakAfterConfig(true)`, and AP +
   save callbacks for logging.
3. `wm.autoConnect("cores3-hydro-setup-<last4mac>")` is called:
   - If WiFi credentials are already persisted (returning unit),
     WiFiManager reconnects in STA mode without opening the portal and
     returns `true` quickly.
   - If not, it brings up an **open** SoftAP (no password) named
     `cores3-hydro-setup-<last4mac>` on 192.168.4.1, with a DNS responder
     that redirects all lookups to itself (the captive-portal trick), and
     blocks serving an HTML form: SSID scan list + password field. On
     submit it stores the creds, exits AP mode, and connects in STA. On
     3-minute timeout it returns `false`.
4. Display is taken over for the duration of the portal with a static
   screen that shows the AP name, the URL (`http://192.168.4.1/`), and a
   timeout hint.
5. On `true` return, `net_begin()` proceeds (WiFi event handler has
   already flipped to `Connected` and `start_services()` is queued).
   On `false`, `net_begin()` reboots — next boot re-enters the portal.

**Credential storage:** Owned by WiFiManager / the arduino-esp32 WiFi
stack (native ESP32 WiFi config). The project's old `wifi` Preferences
namespace from the QR-setup era is **not used**; `wifi_creds_clear()`
wipes it once on reset for migration cleanup but never writes to it.

**Re-entering setup on a configured device:**

- **Touch gesture at boot:** the `check_credential_reset()` path in
  `cores3-hydro.ino` watches for a long-touch in the top-right 60×60 zone
  during the first 3 seconds of boot. On detect it calls
  `net_reset_credentials()`, which calls `wifi_creds_clear()` and
  reboots into the portal on the next boot.
- **HTTP:** `POST /wifi/reset` from any LAN-connected client has the
  same effect. Useful for remote re-setup without physical access.
- **Home-page button:** the admin section of `GET /` exposes a
  "Reconfigure WiFi" button that POSTs to `/wifi/reset` after a
  `confirm()` dialog.

**Implementation files:**

- `wifi_setup.{cpp,h}` — thin WiFiManager wrapper. Exposes
  `wifi_setup_run()` and `wifi_creds_clear()`. No quirc, no NVS storage
  of its own.
- `tzapu/WiFiManager` library — installed via `setup.ps1`. No version
  pin; tracks arduino-cli's resolution.

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
  CAMERA_FB_IN_PSRAM`); large response buffers (e.g. the buffered OTA
  upload image, up to ~1.5 MB) should use `ps_malloc()` or route through
  `heap_caps_*(..., MALLOC_CAP_SPIRAM)`. Keep the ~320 KB internal SRAM
  for tasks, stacks, and atomics — display canvas at 320×240 16 bpp is
  153 KB and goes to PSRAM via `M5Canvas::setPsram(true)`.
- **Library list** (installed via arduino-cli; versions pin to whatever
  `arduino-cli lib install` resolves — no project-level pinning):
  - `m5stack/M5Unified` — display, touch, IMU, PMIC, speaker, mic
  - `m5stack/M5GFX` — pulled in by M5Unified, provides M5Canvas
  - `bblanchon/ArduinoJson` (v7) — JSON serialization
  - `milesburton/DallasTemperature` + `paulstoffregen/OneWire` —
    DS18B20 over 1-Wire on GPIO 8
  - DHT20 read inline in `dht20.cpp` — no library needed; protocol is
    simple enough to keep in 30 lines and saves a dependency
  - LTR-553 read inline in `light.cpp` via `M5.In_I2C` — no library needed
  - `tzapu/WiFiManager` — captive-portal AP for first-time WiFi setup
  - `espressif/esp32-camera` — bundled with the M5Stack arduino-esp32 core
  - `Preferences` — bundled, used for sim_state and device_name NVS state
    (WiFi creds are owned by WiFiManager / arduino-esp32's native config)
- **HTTP library:** `WebServer.h` (synchronous) — not AsyncWebServer. Single
  client, no benefit from async.
- **JSON serialization:** ArduinoJson v7's stack-allocated `JsonDocument`
  sized for the response (~256 bytes for `/sensors` and `/status`). Avoid
  `String` concatenation entirely — it fragments the heap, and this device
  needs to run for months between reboots.
- **WiFi setup:** tzapu/WiFiManager captive-portal AP. See *WiFi Setup
  Mode (WiFiManager)* section above for flow. (v4 through v7 used a
  QR-code scan via the camera; v8 reverted to WiFiManager because QR
  decode reliability was poor in real-world use.)
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
  at file scope in `cores3-hydro.ino`. Default 8 KB is too small for the
  WiFiManager captive portal (its HTTP server, DNS responder, and HTML
  rendering) running during `setup()` on the loop task.
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
- **OTA: two parallel paths.**
  - **ArduinoOTA (push from build host)** — port 3232 via mDNS, brought
    up alongside mDNS in `net.cpp::start_services()`. Same
    reconnect-restart story as mDNS — re-initialized on every WiFi
    reconnect. Driven from `build.ps1 -Network` which passes
    `--protocol network --port <hostname>.local` to `arduino-cli upload`.
    This is the dev workflow.
  - **HTTP browser upload** — `GET /ota` + `POST /ota/upload`, owned by
    `ota.{cpp,h}`. Implements the buffer-then-burn pattern
    (`ps_malloc → receive → validate → Update.write → ESP.restart()`)
    so torn uploads can't brick the device. Releases the camera
    framebuffers via `camera_stop()` at upload start to make PSRAM
    room for the firmware buffer. Sensor tasks idle while
    `g_state.ota_in_progress` is set; the display paints a takeover
    screen. Browser-friendly so operators can update from a phone.
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
├── ds18b20.{cpp,h}         // DS18B20 reader (Port B, GPIO 8, 1-Wire)
├── light.{cpp,h}           // LTR-553ALS reader (internal bus via M5.In_I2C)
├── battery.{cpp,h}         // M5.Power.getBatteryLevel poll task
├── camera.{cpp,h}          // GC0308 init + /snapshot capture path
├── wifi_setup.{cpp,h}      // WiFiManager wrapper: portal + creds-clear
├── http_server.{cpp,h}     // Sync WebServer + handlers
├── ota.{cpp,h}             // GET /ota + POST /ota/upload (PSRAM-buffered)
├── net.{cpp,h}             // WiFi connect, reconnect, mDNS, ArduinoOTA, NTP
├── simulation.{cpp,h}      // random generators (sim_air_temp, etc.)
├── sim_state.{cpp,h}       // NVS persistence for per-sensor sim overrides
├── display.{cpp,h}         // M5Canvas-backed status screen
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
.\build.ps1                          # compile prod (default)
.\build.ps1 -Env no-display          # no display task
.\build.ps1 -Upload -Monitor         # full cycle (serial flash + monitor)
.\build.ps1 -Network                 # OTA push to cores3-hydro.local (ArduinoOTA)
.\build.ps1 -Network -NetHost <host> # OTA push to a different mDNS hostname
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
