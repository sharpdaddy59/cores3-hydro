# CLAUDE.md — cores3-hydro project notes for Claude Code

Hydroponic monitoring firmware for the M5Stack CoreS3 (ESP32-S3, 8 MB OPI
PSRAM, 16 MB flash, 320×240 IPS touch, GC0308 camera, AXP2101 PMIC). Reads
water temp (DS18B20), air temp + humidity (DHT20), and ambient light
(LTR-553ALS); serves them over HTTP/JSON; ships a glanceable display
dashboard. Stateless data source — never alerts, never decides; the upstream
agent (currently OpenClaw) does all interpretation. The HTTP contract is
load-bearing for that agent — keep it stable across changes.

**Authoritative design doc:** `docs/cores3-firmware-spec.md` (currently v5).
Read it for any non-trivial work.

## Build / flash / monitor

PowerShell, from the project root:

```powershell
.\build.ps1                          # compile only (prod env)
.\build.ps1 -Upload                  # compile + auto-detect port + flash
.\build.ps1 -Upload -Monitor         # ... + open serial @ 115200
.\build.ps1 -Env no-display -Upload  # build without the display task
.\build.ps1 -Strict                  # warnings=all (use after touching many files)
```

One-time setup: `.\setup.ps1` installs arduino-cli, the M5Stack board
package, and required libraries.

**Why arduino-cli not PIO:** PlatformIO's mainstream arduino-esp32 doesn't
reliably init OPI PSRAM on this board — camera framebuffers fail to
allocate. We use M5Stack's arduino-esp32 fork via arduino-cli
(`m5stack:esp32:m5stack_cores3` FQBN). This is non-negotiable; do not
migrate back to PIO.

## Hardware map

| Subsystem | Bus | Pin / addr | Notes |
|-----------|-----|------------|-------|
| DHT20 air temp + humidity | Port A I²C (`Wire`) | 0x38 | GPIO 2 SDA, GPIO 1 SCL — caller must `Wire.begin(2, 1)` |
| DS18B20 water temp | Port B 1-Wire | GPIO 8 | Grove unit has integrated 4.7k pullup |
| LTR-553ALS light | Internal I²C (`Wire1`) | 0x23 | Shares bus with camera SCCB |
| GC0308 camera | Internal I²C SCCB + parallel | — | RGB565 VGA, no hardware JPEG |
| Display | M5GFX | 320×240 ILI9342C | Double-buffered via M5Canvas |
| Touch | FT6336U | — | Used for credential-reset gesture |
| Battery / power | AXP2101 PMIC | — | Reported via `M5.Power` |

The DIN base provides Port B (water sensor lives there). Removing the base
requires either a different M5 base or migrating water temp to an I²C
sensor on Port A.

## Architecture pointers

- **Boot order** (`cores3-hydro.ino`): `M5.begin()` → `sim_state_load()` →
  `device_name_init()` → credential-reset gesture window →
  `camera_start()` (early, before WiFi fragments PSRAM) → `net_begin()` →
  sensor tasks → `http_server_begin()` → `display_start()`.
- **Concurrency:** sensor threads own their data; cross-core sharing via
  `std::atomic` in `sensors.h`. `g_wire_mutex` guards `Wire` (Port A);
  `M5.In_I2C` arbitrates `Wire1` between camera and LTR.
- **HTTP:** synchronous `WebServer` polled from `loop()` via
  `http_server_loop()`. Routes registered in `http_server_begin()`.
- **NVS state, all separate namespaces:** WiFi creds (`wifi_setup.cpp`,
  ns `wifi`); per-sensor sim flags (`sim_state.cpp`, ns `sim`); device
  hostname override (`device_name.cpp`, ns `device`).
- **mDNS:** runtime hostname from `device_hostname()`. Default is
  `cores3-hydro-<last4mac>` so multiple units are uniquely discoverable.
  User can rename via the home page or `POST /hostname`; persisted in NVS.

## Critical gotchas

1. **Camera + LTR-553 share `Wire1`.** Camera init must
   `M5.In_I2C.release()` first; LTR reads use
   `M5.In_I2C.readRegister/writeRegister` at **100 kHz**. Do NOT call
   `Wire1.end()` / `Wire1.begin()` — that breaks the camera. Topology in
   `camera.cpp` and `light.cpp`.
2. **PSRAM is fragile if camera inits late.** `camera_start()` must run
   early in `setup()`, before WiFi and other heavy consumers. Camera frame
   buffers need a contiguous PSRAM allocation.
3. **First-time WiFi setup is WiFiManager AP + captive portal.** Device
   opens `cores3-hydro-setup-<last4mac>` (open SoftAP); user browses to
   192.168.4.1, picks an SSID, enters the password. Portal times out at
   3 min. Credentials live in the ESP32's native WiFi config, NOT in the
   project's old `wifi` Preferences namespace (that's wiped on reset for
   migration cleanup only).
4. **`build.extra_flags` overwrites board defaults.** Use
   `compiler.cpp.extra_flags` instead — `build.ps1` already does this.
   Setting `build.extra_flags` silently strips `ARDUINO_M5STACK_CORES3` /
   `BOARD_HAS_PSRAM` and breaks the build in confusing ways.
5. **Loop task stack** is raised to 16 KB via
   `SET_LOOP_TASK_STACK_SIZE(16 * 1024)` in the .ino. Required because
   WiFiManager's captive portal (HTTP + DNS + HTML rendering) runs on
   the loop task during setup.
6. **Touch credential-reset gesture** has 800 ms settle + 150 ms
   sustained-press requirement to avoid false positives. Don't shorten
   without testing — early versions fired spuriously.

## Conventions for new work

- **New sensor:** mirror the `dht20` / `ds18b20` / `light` pattern — own
  header + cpp, `xxx_start()` spawns a FreeRTOS task, writes to atomics
  in `g_state`. Add a runtime sim path
  (`if (g_state.simulate_xxx.load()) ...`) and `sim_state.cpp` save/load
  if it should be toggleable from the web UI.
- **New HTTP endpoint:** add to `http_server.cpp`, register in
  `http_server_begin()`, document it in
  `docs/cores3-firmware-spec.md` under the HTTP API section. The agent
  reads from this contract — don't break existing field names without a
  matching FW_VERSION bump and changelog note.
- **New NVS-persisted state:** mirror `sim_state.cpp` or
  `device_name.cpp` — separate Preferences namespace, load in `setup()`,
  save inline on change.
- **User-facing changes:** bump `FW_VERSION` in `config.h`, add a note to
  the changelog at the top of `docs/cores3-firmware-spec.md`.
- **Spec doc is the source of truth** for design decisions. If behavior
  diverges from the spec, update the spec.

## Don'ts

- Don't reintroduce `secrets.h` or hardcoded WiFi credentials. The
  WiFiManager captive portal is the one true path.
- Don't migrate back to PlatformIO. PSRAM init is broken there.
- Don't disable the watchdog. Hung sensor threads should reboot.
- Don't add authentication assumptions to HTTP endpoints — the device is
  LAN-trusted by design.
- Don't `Wire1.end()` or `Wire1.begin()` — you'll desync camera/LTR bus
  arbitration.
- Don't bump `FW_VERSION` without also updating the spec changelog.

## Recent state

- **v0.7.1 (current):** Fix the "Mounted upside down" feature — the v0.6.0
  implementation relied on `set_hmirror(1) + set_vflip(1)` to produce a
  180° rotation, but the esp32-camera GC0308 driver silently drops the
  hmirror write on this hardware (the sensor's `status.hmirror` field
  reflects the request, but the captured pixels don't change; `vflip`
  works fine). So an actually-upside-down unit got a horizontally
  mirrored `/snapshot` instead of a clean R180. Fix is to do the rotation
  **in software** in `camera_get_frame()`: when `g_state.display_flipped`
  is set, the RGB565 framebuffer is reverse-walked in-place (~15-30 ms
  for VGA on the S3, fine for an on-demand endpoint). Sensor is left at
  `hmirror=0, vflip=0`; `camera_set_flip()` becomes a documented no-op
  kept as the entry point so `/display` doesn't need to know how the
  flip is implemented. Diagnostic UI we added to isolate this
  (`Camera (diagnostic)` section + `/cam_raw` endpoint +
  `camera_set_raw` / `camera_get_raw_bits`) is removed; if you ever
  need it again, look at the v0.7.1 commit. Home page also gains port
  labels in the sensor list (`DHT20 on Port A`, `DS18B20 on Port B`,
  `LTR-553ALS (internal)`).
- **v0.7.0:** User-selectable temperature units (Celsius /
  Fahrenheit) and a third sensor mode: **disabled**. Units live in a
  new `units.{cpp,h}` module (NVS namespace `units`, key `temp`, uint8
  0=C / 1=F, default Fahrenheit). Both the dashboard and `/sensors`
  emit temperatures in the selected unit — the device converts on
  egress so OpenClaw doesn't have to. `/sensors` gains a
  `temperature_units` field. New endpoint: `GET`/`POST /units`. Sensors
  are now tristate: `simulate_<x>` bools are replaced with `<x>_mode`
  atomics backing `SensorMode { REAL, SIMULATED, OFF }` (the C++
  identifier `OFF` only exists to dodge ESP32's `#define DISABLED`
  GPIO-mode macro — the wire-format string is still `"disabled"`).
  DISABLED is for "I'm not using this sensor and that's fine" (e.g.
  microgreens with no water tank); the task short-circuits its read
  loop, and `/sensors` emits `null` + `status: "disabled"`. **Breaking
  HTTP changes:** `/sensors` replaces the `simulated` boolean object
  with a `status` object of `"real"`/`"simulated"`/`"disabled"`
  strings; `/sim`'s POST grammar changes from `on/off/true/false/1/0`
  to `real/sim/simulated/disabled/off/none` (the old `off=real`
  shorthand would have collided with the new `off=disabled`). Existing
  NVS `sim` keys migrate silently: `getUChar` decodes the old bool
  values (false=0=REAL, true=1=SIMULATED). Home page gains a
  Temperature-units section; sensor toggles become 3-way dropdowns.
  Display shows `OFF` in dark grey for disabled sensors and the unit
  suffix (`F`/`C`) reflects the user choice.
- **v0.6.0:** User-toggleable "Mounted upside down" orientation
  flip on the home page. When enabled, display rotation switches from
  `1` → `3` (landscape, 180°) and the GC0308 sensor's hmirror+vflip are
  both set so `/snapshot` images match the screen. Use case: mount the
  CoreS3 inverted so the Grove cable on Port A (the top of the chassis)
  hangs down behind the unit instead of draping across the screen.
  Persisted in NVS via the existing `display` namespace (new key
  `flip`); exposed as `flipped` (bool) on `GET`/`POST /display`. New
  `camera_set_flip(bool)` in `camera.cpp` writes hmirror+vflip over
  SCCB at runtime, holding the capture mutex so it can't race with an
  in-flight `/snapshot`. `display_settings_load()` now runs right after
  `M5.begin()` so the very first `setRotation()` picks up the stored
  orientation — otherwise the boot screen would briefly show in the
  wrong orientation.
- **v0.5.0:** Reverted first-time WiFi setup from QR-code
  scanning back to tzapu/WiFiManager (open SoftAP at
  `cores3-hydro-setup-<last4mac>`, captive portal at
  http://192.168.4.1/, 3-minute timeout). QR turned out to be too
  unreliable in practice — phone-screen moiré against the GC0308 Bayer
  pattern made decode flaky. Camera is kept (still used by
  `/snapshot`). Quirc and the scan loop are deleted entirely; no
  fallback. WiFi credentials now live in the ESP32's native WiFi
  config (owned by WiFiManager); the legacy `wifi` Preferences namespace
  is wiped on credential reset for migration cleanup. Home-page admin
  button renamed "Reconfigure WiFi"; `POST /wifi/reset` response
  message names the new AP. `wifi_setup.{cpp,h}` is now a thin
  WiFiManager wrapper.
- **v0.4.0:** Display gains user-configurable backlight brightness +
  idle-driven dim/sleep timers (NVS-persisted via new `display`
  namespace; `GET`/`POST /display`). Home page exposes brightness slider
  + dim/sleep number inputs. Touch wakes the display.
- **v0.3.0:** HTTP-based OTA firmware update. `GET /ota` serves
  an upload page; `POST /ota/upload` buffers the full `.bin` in PSRAM via
  `ps_malloc` before any byte hits flash, so a torn upload leaves the
  running partition untouched. Camera framebuffers are released at upload
  start to free PSRAM; sensor tasks idle on a new `g_state.ota_in_progress`
  flag; display switches to a takeover screen. The existing ArduinoOTA
  push path is preserved. `build.ps1` gains `-Network` (push via
  ArduinoOTA / mDNS) and emits `--export-binaries` so the `.bin` path is
  predictable. Implementation lives in `ota.cpp` / `ota.h`.
- **v0.2.0:** per-device mDNS hostname with NVS-persisted user override,
  `GET`/`POST /hostname` endpoints, home-page UI for renaming. Display
  header and browser tab title both reflect the live hostname. Default
  hostname includes a per-MAC suffix so multiple units are uniquely
  discoverable. Repo lives at
  `https://github.com/sharpdaddy59/cores3-hydro` (renamed from
  `Plant-Monitor`; fresh history — the pre-v0.2.0 PIO→arduino-cli
  migration noise was squashed out).
- **v0.1.0 history** (squashed): initial scaffolding, PIO→arduino-cli
  migration, QR-based WiFi setup, per-sensor runtime simulation
  overrides, interactive home-page UI.

Future hardware to test: M5Stack CoreS3 Lite + a separate DIN base. Spec
sheet suggests the silicon is identical to the original CoreS3, so the
firmware should run unchanged, but verify camera SCCB / LTR coexistence
on first boot since that's the most fragile bus topology in the project.

## Where to look first

- `cores3-hydro.ino` — boot orchestration, FreeRTOS task spawn
- `docs/cores3-firmware-spec.md` — full design doc + HTTP API reference
- `config.h` — central tunables (intervals, pins, version)
- `sensors.h` — global state, atomic flags
- `http_server.cpp` — all HTTP handlers + the home-page HTML/CSS/JS
- `net.cpp` — WiFi reconnect machine, mDNS / OTA / NTP plumbing
