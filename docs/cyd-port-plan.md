# CYD port plan

**Status:** proposal / future work — not yet implemented.

A plan for porting cores3-hydro firmware to the Sunton ESP32-2432S028R
(CYD) board, dropping camera + ambient-light support in exchange for a
~5× cost reduction per growing-station node and a built-in display.

## Why

| Metric | CoreS3 + DIN base | CYD-S028R |
|---|---|---|
| Hardware cost | ~$70 | ~$12 |
| Built-in display | 2.0" 320×240 | 2.8" 320×240 |
| Built-in camera | Yes (GC0308) | No |
| Built-in LDR | Yes (LTR-553ALS, reliable) | Sometimes (pop-on-pop-off, GPIO 34) |
| Free GPIOs after firmware | Plenty (Port A, Port B) | Three (GPIO 22, 27, 35) + speaker (26) repurposable |
| WiFi onboarding path | QR via camera | WiFiManager AP-mode |

For a multi-station setup (the user has 3–4 CYDs lying around), the
cost difference is ~$200+. The CYD also obviates a separate hydro-dash
unit per station — each CYD-hydro can render its own readings locally
on the 2.8" panel.

## Scope

**Keep:**
- DHT20 (air temp + humidity, I²C)
- DS18B20 (water temp, 1-Wire)
- HTTP `/sensors` and `/status` JSON contract — preserved verbatim so
  existing hydro-dash dashboards auto-discover CYD-hydro units
  alongside CoreS3-hydro units (discovery is shape-based, not hostname-
  prefix-based, since hydro-dash v0.1.2)
- Per-sensor sim flags + persistence
- mDNS hostname (`cyd-hydro-<last4mac>` or unify with `cores3-hydro-`
  prefix — TBD)
- HTTP-based OTA (the v0.3.0 cores3-hydro feature — PSRAM-buffered
  upload). CYD's WROOM-32 has 4 MB Flash and ~520 KB SRAM; no OPI
  PSRAM, but enough flash to buffer a typical .bin.

**Drop:**
- GC0308 camera + quirc QR decoder. WiFiManager AP-mode replaces
  QR-based credential entry.
- LTR-553ALS ambient-light sensor. The CYD's LDR on GPIO 34 is a
  per-unit lottery (some work, some don't); make it a `USE_LDR`
  compile-time toggle and skip if missing.
- Touch credential-reset gesture (no touch driver overhead during
  startup; user can reset via the existing `POST /wifi/reset` endpoint
  or the on-device settings screen if we add the merged-UI variant).

**Optional bonus:**
- **Merged firmware variant.** Combine cores3-hydro's sensor logic
  with hydro-dash's hero view in one sketch: same board reads its
  own sensors and shows them on its own LCD. Removes the need for a
  separate dashboard. Two SCAD `mode` variants (`SENSOR_ONLY` /
  `SENSOR_PLUS_UI`) gated by a compile-time flag.

## Pin map

CYD usable pins (per Random Nerd's pinout):
- **CN1**: GND, GPIO 22, GPIO 27, 3V3 — perfect for a 4-wire I²C
  sensor (VCC/GND/SDA/SCL).
- **P3**: GND, GPIO 35 (input-only), GPIO 22, GPIO 21 (backlight!).
  GPIO 21 is unusable without losing the screen; GPIO 35 is input-
  only, so 1-Wire (which is bidirectional) won't work there.
- **Speaker JST**: GND, GPIO 26 — repurposable. GPIO 26 is
  bidirectional, fine for 1-Wire.

| Sensor | Bus | CYD pin(s) | Connector | Notes |
|---|---|---|---|---|
| DHT20 | I²C | SDA = **GPIO 22**, SCL = **GPIO 27** | CN1 | 3V3 + GND also on CN1; clean four-wire connection. |
| DS18B20 | 1-Wire | DATA = **GPIO 26** | Speaker JST | 4.7 kΩ pull-up to 3V3 required; pulls VCC + GND from CN1 or a separate header. |
| LDR (optional) | ADC | **GPIO 34** | On-board | Skip via `USE_LDR=false` if your unit reads zero (verify with the diagnostic test from hydro-dash's history). |

This consumes all three "free" pins (22, 27, 26). GPIO 35 stays free
for one input-only future addition (e.g., flow sensor pulse counter).

## What changes vs cores3-hydro

```
cores3-hydro/
├── camera.{cpp,h}         <- DELETE (no camera)
├── quirc.*, decode.c, ... <- DELETE (no QR decoder)
├── light.{cpp,h}          <- ADAPT (LTR-553 → optional CYD LDR)
├── wifi_setup.{cpp,h}     <- REWRITE (QR flow → WiFiManager AP)
├── dht20.{cpp,h}          <- KEEP, repin (Wire pins 22/27)
├── ds18b20.{cpp,h}        <- KEEP, repin (GPIO 26 + 4.7kΩ)
├── http_server.{cpp,h}    <- KEEP (HTTP API contract preserved)
├── ota.{cpp,h}            <- KEEP (HTTP OTA, no PSRAM buffer needed
│                              — use heap or split into chunks)
├── display.{cpp,h}        <- ADAPT (M5GFX → LovyanGFX, port the
│                              hydro-dash CYD panel config)
├── sim_state.{cpp,h}      <- KEEP verbatim
├── device_name.{cpp,h}    <- KEEP, default prefix → "cyd-hydro-"
├── net.{cpp,h}            <- KEEP (mDNS / OTA / NTP plumbing)
└── cores3-hydro.ino       <- ADAPT (drop camera/touch boot steps,
                                add WiFiManager step, keep sensor
                                threads + http server + display)
```

The HTTP `/sensors` JSON shape stays identical:

```json
{
  "water_temp": 22.5,
  "air_temp":   24.1,
  "humidity":   55,
  "light":      320,           // omit or null if USE_LDR=false
  "rssi":       -45,
  "wifi_ok":    true,
  "ss_boot_water": 9120, "ss_boot_air": 9123, "ss_boot_light": 9100,
  "simulated":  { "air": false, "water": false, "light": false }
}
```

If `USE_LDR=false`, `light` is omitted (or always reports the sim
default). hydro-dash's hero view already handles `NaN` readings
gracefully — they show as `--`.

## Ota note

cores3-hydro's HTTP OTA buffers the full `.bin` in PSRAM via
`ps_malloc` before writing flash. The CYD-WROOM has no PSRAM. Two
options for the port:

1. **Heap-buffered OTA**: a typical hydro firmware build is ~1.3 MB,
   the WROOM has ~320 KB heap → won't fit. Reject.
2. **Stream OTA**: write each upload chunk to flash as it arrives.
   Loses the "atomic abort" property (a torn upload corrupts the
   inactive partition), but flash partitioning means the running
   image stays untouched until the boot pointer flips. Safer than
   it sounds.
3. **ArduinoOTA push only**: skip the browser HTTP upload page and
   keep the `arduino-cli upload --protocol network` path. Simpler
   but you can't update from a phone.

Recommendation: option 2 (streaming HTTP OTA), with the existing
ArduinoOTA push path also retained. Same UX as cores3-hydro just
without the PSRAM buffering safety net.

## Hostname / discovery

Two choices for hostname prefix:

- **`cyd-hydro-<last4mac>`** — distinguishes hardware variant in
  mDNS browse output. Useful for ops.
- **`hydro-<last4mac>`** — unifies CoreS3 and CYD units under one
  prefix, treats them as fungible from the dashboard's perspective.

Either works since hydro-dash v0.1.2 stopped relying on the
`cores3-hydro-` prefix and probes `/sensors` shape instead. I'd
go with the first for diagnosability ("which one is the cheap one?")
without breaking discovery.

## Hardware BOM (per station)

| Item | Qty | Cost | Notes |
|---|---|---|---|
| CYD-S028R | 1 | ~$12 | AliExpress / Sunton |
| DHT20 module (Grove or bare) | 1 | ~$3 | I²C, 3V3 |
| DS18B20 waterproof probe | 1 | ~$3 | Pre-wired, 3-wire, internal pull-up may or may not exist |
| 4.7 kΩ resistor | 1 | <$0.10 | Pull-up for DS18B20 if probe doesn't have one |
| JST 1.25 mm pigtail (4-wire for CN1, 2-wire for speaker JST) | 2 | ~$2 | Sunton uses small JSTs not breadboard headers |
| 3D-printed enclosure | 1 | <$1 | Adapt hydro-dash's enclosure SCAD; add cable gland for sensor leads |
| **Total** | | **~$21** | vs ~$80+ for CoreS3 + DIN + Grove cables |

## Open questions / verify before starting

1. **DHT20 + GPIO 22/27 I²C reliability.** GPIO 22 is "share-ish" with
   the touch CS pin in some board revs — verify on the user's
   specific unit before designing the PCB stencil into the case.
2. **GPIO 26 1-Wire pull-up.** Some DS18B20 modules ship with built-in
   4.7 kΩ; many bare probes don't. Add the resistor on the pigtail or
   inside the enclosure.
3. **OTA partition layout.** Need to confirm `min_spiffs` partition
   scheme leaves room for two app slots + the firmware. The hydro
   firmware is bigger than hydro-dash because of sensor drivers, so
   flash-budget pressure is real.
4. **Camera-less boot timing.** cores3-hydro's boot sequence puts
   `camera_start()` very early to avoid PSRAM fragmentation. Without
   camera + PSRAM, that whole concern goes away — but verify the
   sensor-thread spawn order still works on a smaller heap.
5. **Display takeover during OTA.** cores3-hydro's `display.cpp` shows
   an OTA progress screen during firmware upload. The port should
   keep this for user feedback.
6. **Stylus.** The CYD has touch but for a sensor-station role, no
   touch interaction is needed. The cores3-hydro firmware has a
   `wifi-credential-reset` touch gesture; the port could keep it
   (XPT2046 driver via LovyanGFX) or drop it in favour of the HTTP
   `/wifi/reset` endpoint.

## Migration / coexistence

A user already running CoreS3-hydro stations can deploy CYD-hydro
stations alongside without changing anything else:

- mDNS browse picks up both prefixes.
- hydro-dash dashboard auto-discovers any device whose `/sensors`
  returns the expected JSON shape.
- The cores3-hydro CLAUDE.md memory file can stay as the source of
  truth for the original platform; a new CLAUDE.md for the CYD port
  documents the differences.

## Suggested first session

1. New repo or branch (`cores3-hydro/cyd-port` branch, then split if
   it gets too divergent).
2. Strip camera + quirc files. Rip out the touch-gesture code.
3. Replace `wifi_setup.cpp` with WiFiManager (port verbatim from
   hydro-dash).
4. Repin `dht20.cpp` and `ds18b20.cpp`.
5. Wire up `display.cpp` from hydro-dash's panel config (the
   panel-width-swap + offset_y=80 + rotation 4 + rgb_order=true
   incantation we figured out the hard way).
6. Compile with `arduino-cli` against `esp32:esp32:esp32` FQBN.
7. Smoke test on bench: bare CYD + DHT20 on CN1 + DS18B20 on
   speaker JST. Verify `/sensors` returns sane values.
8. Decide on the merged-UI variant — flip a compile flag, test the
   on-device hero view.

## Where this would live

Either:

- **A new repo `cyd-hydro`** (sibling to `cores3-hydro` and
  `Hydro-Desktop`), so its README / CI / releases are independent.
- **A `cyd/` subdirectory in cores3-hydro** with its own .ino, sharing
  config.h drivers via include path.

I'd lean new repo — different hardware, different boot order, and the
cores3-hydro firmware has a lot of CoreS3-specific code (M5Unified,
AXP2101 power management) that doesn't apply. Cleaner separation.
