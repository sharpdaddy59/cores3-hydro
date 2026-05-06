# cores3-hydro

Hydroponic monitoring firmware for the [M5Stack CoreS3](https://docs.m5stack.com/en/core/CoreS3)
(ESP32-S3). Reads water temperature, air temperature & humidity, and
ambient light; publishes them over a JSON HTTP API; shows a glanceable
dashboard on the built-in display; and exposes a `/snapshot` endpoint
that streams the current camera frame.

The device is a **stateless data source**. It never alerts, never makes
decisions — it just publishes readings. All the interpretation happens in
an upstream agent (in my case, an LLM-driven monitor that watches the
readings and tells me whether the plants are happy).

## Features

- Live sensor readings over JSON (`/sensors`, `/status`)
- Camera snapshot endpoint (`/snapshot`) serving the current frame as JPEG
- **QR-code Wi-Fi setup** — hold a Wi-Fi-format QR on a printed page or
  laptop screen up to the camera; no captive portal, no app
- **Per-sensor simulation overrides** — flip any sensor into fake-data
  mode from the web UI for testing, demos, or quarantining a misbehaving
  sensor; persisted in NVS
- **Renameable mDNS hostname** — defaults to `cores3-hydro-<last4mac>.local`
  so multiple units on the same LAN are uniquely discoverable; rename
  from the web UI
- Glanceable display with sim-mode tinting, time/date, Wi-Fi/battery state
- **Web-based firmware updates** — drag a new `.bin` onto `/ota` in your
  browser; the upload is buffered in PSRAM before flashing so a dropped
  connection leaves the running firmware untouched. Push-style ArduinoOTA
  via `arduino-cli` is also supported (`build.ps1 -Network`).
- NTP-synced timekeeping with timezone support

## Hardware

- M5Stack CoreS3 (or CoreS3 Lite) + a DIN base (any M5 base that exposes
  Port B works; the DIN base is what I use)
- M5Stack Grove **DS18B20** 1-Wire waterproof probe → Port B (water temp)
- M5Stack Grove **DHT20** sensor → Port A (air temp + humidity)
- The CoreS3's built-in LTR-553ALS ambient light sensor and GC0308 camera

## Quick start

PowerShell on Windows:

```powershell
git clone https://github.com/sharpdaddy59/cores3-hydro.git
cd cores3-hydro

# One-time setup: installs arduino-cli, the M5Stack board package,
# and required libraries.
.\setup.ps1

# Plug in the CoreS3 over USB-C, then:
.\build.ps1 -Upload -Monitor
```

On first boot the device prompts you to scan a Wi-Fi-format QR code:

```
WIFI:T:WPA;S:<your-ssid>;P:<your-password>;;
```

Generate one in any QR generator, display it on a laptop screen or
printed page, and hold it in front of the camera. The device decodes it,
saves the credentials, and connects. Phone screens tend to cause moiré
aliasing — use a printed or laptop-screen QR for reliable results.

Once connected, visit `http://cores3-hydro-<last4mac>.local` in a
browser. The full hostname is shown on the device's display.

## Web UI

The home page provides:

- Live data links (`/sensors`, `/status`, `/snapshot`)
- Toggle switches to put each sensor into simulation mode
- A device-name editor — rename and the new `.local` URL takes effect
  immediately (other devices' caches may take ~2 minutes to forget the
  old name)
- A "Reset Wi-Fi credentials" button to re-enter QR setup mode

## HTTP API

| Endpoint | Methods | Description |
|----------|---------|-------------|
| `/` | GET | Interactive HTML control panel |
| `/sensors` | GET | Latest readings + per-sensor sim flags (JSON) |
| `/status` | GET | Uptime, RSSI, battery, heap, PSRAM, NTP, FW version |
| `/snapshot` | GET | Current camera frame (image/jpeg) |
| `/sim` | GET, POST | Get / set per-sensor simulation overrides |
| `/hostname` | GET, POST | Get / set the mDNS hostname |
| `/ota` | GET | Firmware-upload page |
| `/ota/upload` | POST | Receive a `.bin`, buffer in PSRAM, flash on success |
| `/wifi/reset` | POST | Wipe stored Wi-Fi credentials and reboot |

Sample `/sensors` response:

```json
{
  "water_temp": 22.5,
  "air_temp":   24.1,
  "humidity":   55,
  "light":      320,
  "rssi":       -45,
  "wifi_ok":    true,
  "ss_boot_water": 9120, "ss_boot_air": 9123, "ss_boot_light": 9100,
  "simulated":  { "air": false, "water": false, "light": false }
}
```

The full reference, including JSON shapes and the rationale for each
design decision, lives in [`docs/cores3-firmware-spec.md`](docs/cores3-firmware-spec.md).

## Updating firmware

Two paths, both go over Wi-Fi — no USB cable needed once the device is
on your LAN.

**Browser upload (easiest):**

1. Build a new image — `.\build.ps1` drops
   `build/m5stack.esp32.m5stack_cores3/cores3-hydro.ino.bin` next to the
   sketch.
2. Open `http://cores3-hydro-<last4mac>.local/ota` in a browser.
3. Pick the `.bin`, hit **Upload**. The device pauses sensor tasks,
   shows a takeover screen, buffers the full image into PSRAM, then
   flashes and reboots. If anything goes wrong before the flash step
   (network drop, browser closed, malformed `.bin`), the running
   partition is untouched.

**Push from the dev machine:**

```powershell
.\build.ps1 -Network                        # default hostname
.\build.ps1 -Network -NetHost greenhouse-1  # named device
```

This uses ArduinoOTA over mDNS — same on-device flash path, just driven
by `arduino-cli` instead of the browser.

## Why arduino-cli, not PlatformIO?

PlatformIO's mainstream arduino-esp32 fork doesn't reliably initialize
the CoreS3's OPI PSRAM, which breaks the camera. This project uses
M5Stack's arduino-esp32 fork via arduino-cli
(`m5stack:esp32:m5stack_cores3` FQBN) instead. The migration story is
described in the spec doc.

## Project layout

```
cores3-hydro/
├── cores3-hydro.ino       Sketch entry point + boot orchestration
├── config.h               Central tunables (intervals, pins, FW_VERSION)
├── sensors.h              Global state, std::atomic flags
├── dht20.{cpp,h}          Air sensor task (Wire / Port A)
├── ds18b20.{cpp,h}        Water sensor task (1-Wire / Port B)
├── light.{cpp,h}          Light sensor task (Wire1, shared with camera)
├── battery.{cpp,h}        AXP2101 battery telemetry
├── camera.{cpp,h}         GC0308 camera driver, frame queue
├── wifi_setup.{cpp,h}     QR-based Wi-Fi onboarding
├── net.{cpp,h}            Wi-Fi reconnect, mDNS, OTA, NTP
├── display.{cpp,h}        Off-screen-canvas dashboard
├── http_server.{cpp,h}    Routes + home page UI
├── ota.{cpp,h}            HTTP firmware upload (PSRAM-buffered)
├── sim_state.{cpp,h}      Per-sensor sim flag persistence
├── device_name.{cpp,h}    mDNS hostname management
├── quirc.{c,h}, decode.c, identify.c, version_db.c, quirc_internal.h
│                          Vendored quirc QR decoder (PSRAM-patched)
├── build.ps1, setup.ps1   arduino-cli wrappers
├── docs/cores3-firmware-spec.md
│                          Full design + endpoint reference
└── CLAUDE.md              Notes for Claude Code agents working on this repo
```

## Status

v0.3.0 — running on a single CoreS3 + DIN base in production. Next on
the list: a side-by-side test with the CoreS3 Lite to confirm the
firmware ports unchanged.

## License

MIT — see [`LICENSE`](LICENSE). Bundled quirc QR decoder is ISC-licensed
(© Daniel Beer); the notice is preserved in those source files.
