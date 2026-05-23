// http_server.cpp — /sensors, /status, /snapshot handlers.
//
// JSON serialization uses ArduinoJson v7 (dynamic JsonDocument). For this
// device's tiny payload sizes, the dynamic allocation is cheap and avoids
// hard-coding sizes per response.

#include <Arduino.h>
#include <M5Unified.h>      // for M5.Power battery diagnostic in /status
#include <WebServer.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

#include <cmath>

#include "http_server.h"
#include "sensors.h"
#include "camera.h"
#include "config.h"
#include "wifi_setup.h"    // for wifi_creds_clear() in /wifi/reset
#include "sim_state.h"     // for sim_state_save_* in /sim
#include "units.h"         // user-selected temperature unit (C/F)
#include "device_name.h"   // hostname getters/setter for /hostname
#include "display_settings.h"  // for display_settings_save_* in /display
#include "net.h"           // net_apply_hostname_change() after rename
#include "ota.h"           // ota_register() — /ota and /ota/upload handlers

static WebServer s_server(HTTP_PORT);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void emit_float_or_null(JsonDocument &doc, const char *key, float v, bool fresh) {
  if (!fresh || std::isnan(v)) doc[key] = nullptr;
  else                          doc[key] = v;
}

// ---------------------------------------------------------------------------
// /sensors
//
// Per-sensor mode decides what we emit for each value:
//   REAL or SIMULATED: the stored value, but only if the timestamp is fresh
//     (older readings collapse to null so the agent doesn't act on stale data).
//   DISABLED: null — the user explicitly turned this sensor off, so there is
//     no meaningful value to report.
//
// Temperatures are converted to the user-selected unit. The `temperature_units`
// field tells the agent which unit ("celsius" or "fahrenheit") to interpret
// water_temp / air_temp in for this response. The per-sensor `status` object
// disambiguates a null value: "real" | "simulated" | "disabled".
// ---------------------------------------------------------------------------
static void handle_sensors() {
  uint32_t now = now_seconds_since_boot();

  SensorMode air_mode   = sensor_mode_from(g_state.air_mode.load());
  SensorMode water_mode = sensor_mode_from(g_state.water_mode.load());
  SensorMode light_mode = sensor_mode_from(g_state.light_mode.load());

  bool fresh_air   = reading_is_fresh(g_state.seconds_since_boot_air.load(),   now);
  bool fresh_water = reading_is_fresh(g_state.seconds_since_boot_water.load(), now);
  bool fresh_light = reading_is_fresh(g_state.seconds_since_boot_light.load(), now);

  bool emit_air   = (air_mode   != SensorMode::OFF) && fresh_air;
  bool emit_water = (water_mode != SensorMode::OFF) && fresh_water;
  bool emit_light = (light_mode != SensorMode::OFF) && fresh_light;

  JsonDocument doc;

  // Temperatures: convert raw Celsius to the user's chosen unit. NAN
  // passes through temp_in_user_unit unchanged, so the null branch below
  // still fires for stale or unread values.
  emit_float_or_null(doc, "water_temp",
                     temp_in_user_unit(g_state.water_temp.load()), emit_water);
  emit_float_or_null(doc, "air_temp",
                     temp_in_user_unit(g_state.air_temp.load()),   emit_air);

  float h = g_state.humidity.load();
  if (emit_air && !std::isnan(h)) doc["humidity"] = (int)h;
  else                             doc["humidity"] = nullptr;

  if (emit_light) doc["light"] = g_state.light_lux.load();
  else            doc["light"] = nullptr;

  doc["temperature_units"] = temp_unit_label();

  doc["rssi"]          = g_state.wifi_rssi.load();
  doc["ss_boot_water"] = g_state.seconds_since_boot_water.load();
  doc["ss_boot_air"]   = g_state.seconds_since_boot_air.load();
  doc["ss_boot_light"] = g_state.seconds_since_boot_light.load();
  doc["wifi_ok"]       = g_state.wifi_connected.load();

  // Per-sensor source-of-truth label. "real" = hardware read, "simulated"
  // = sim_*() generator, "disabled" = sensor intentionally off (value is
  // null and the agent should treat the field as not-present rather than
  // missing-due-to-error). Replaces the pre-v0.7.0 boolean `simulated`
  // object.
  JsonObject status = doc["status"].to<JsonObject>();
  status["air"]   = sensor_mode_label(air_mode);
  status["water"] = sensor_mode_label(water_mode);
  status["light"] = sensor_mode_label(light_mode);

  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// /status
// ---------------------------------------------------------------------------
static void handle_status() {
  JsonDocument doc;
  doc["uptime"]      = now_seconds_since_boot();
  doc["rssi"]        = g_state.wifi_rssi.load();
  doc["battery_pct"] = g_state.battery_pct.load();
  doc["heap_free"]   = ESP.getFreeHeap();

  if (g_state.boot_time_epoch != 0) {
    doc["boot_epoch"] = g_state.boot_time_epoch;
    doc["ntp_synced"] = true;
  } else {
    doc["boot_epoch"] = nullptr;
    doc["ntp_synced"] = false;
  }
  doc["wifi_ok"]    = g_state.wifi_connected.load();
  doc["fw_version"] = FW_VERSION;

  // Camera diagnostic — exposed when serial monitor is unreliable.
  doc["camera_ok"]   = camera_is_initialized();
  doc["camera_err"]  = camera_last_error();
  doc["psram_total"] = (uint32_t)ESP.getPsramSize();
  doc["psram_free"]  = (uint32_t)ESP.getFreePsram();

  // Actively probe PSRAM with ps_malloc — the size APIs sometimes return 0
  // even when PSRAM is allocatable, which tells us the SDK has it as memmap
  // rather than heap. If ps_malloc succeeds, PSRAM is usable at runtime even
  // if not via MALLOC_CAP_SPIRAM heap.
  void *probe = ps_malloc(1024);
  doc["psram_alloc_ok"] = (probe != nullptr);
  if (probe) free(probe);

  // Battery diagnostic. Voltage is the most useful signal:
  //   <3000 mV  → deeply discharged or dead LiPo (won't sustain load)
  //   3500-4200 → healthy LiPo (4200 = full, 3500 ≈ near-empty)
  //   ~5000     → reading USB rail through PMIC (no battery attached)
  //   0         → AXP2101 reports no battery present
  doc["battery_mv"]  = (uint32_t)M5.Power.getBatteryVoltage();
  doc["is_charging"] = M5.Power.isCharging();

  String out;
  serializeJson(doc, out);
  s_server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
// /snapshot — JPEG body, no JSON wrapper
//
// camera_get_frame holds an internal mutex on success and pairs with
// camera_release_frame. On failure, the mutex is already released and we
// must NOT call release_frame.
// ---------------------------------------------------------------------------
static void handle_snapshot() {
  CameraFrame f{nullptr, 0, false};

  if (!camera_get_frame(&f, CAMERA_TIMEOUT_MS)) {
    s_server.send(503, "text/plain", "camera unavailable");
    return;
  }

  s_server.setContentLength(f.len);
  s_server.send(200, "image/jpeg", "");
  WiFiClient client = s_server.client();
  client.write(f.jpeg, f.len);

  camera_release_frame();
}

// ---------------------------------------------------------------------------
// GET / — landing page. A user pointing a browser at cores3-hydro.local
// gets a simple list of available endpoints so they don't have to remember
// the paths.
// ---------------------------------------------------------------------------
static void handle_root() {
  static const char *kHome =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <title>cores3-hydro</title>\n"   /* updated by JS from /hostname */
    "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "  <style>\n"
    "    body { font: 15px system-ui, sans-serif; max-width: 560px;\n"
    "           margin: 2em auto; padding: 0 1em; color:#222; }\n"
    "    h1 { margin: 0 0 0.2em; }\n"
    "    h2 { margin-top: 1.6em; border-bottom:1px solid #ddd; padding-bottom:0.2em; }\n"
    "    code { background:#eee; padding:2px 5px; border-radius:3px; }\n"
    "    a { color:#0366d6; text-decoration:none; }\n"
    "    a:hover { text-decoration:underline; }\n"
    "    li { margin: 0.4em 0; }\n"
    "    .muted { color:#666; }\n"
    "\n"
    "    /* Sim mode row */\n"
    "    .sim-row { display:flex; align-items:center; gap:0.8em;\n"
    "               padding: 0.5em 0; border-bottom:1px solid #f0f0f0;\n"
    "               flex-wrap: wrap; }\n"
    "    .sim-row:last-child { border-bottom: none; }\n"
    "    .sim-name { flex:0 0 6em; font-weight: 600; }\n"
    "    .sim-state { flex: 1; font-size: 0.9em; min-width: 12em; }\n"
    "    .sim-state.real     { color: #0a7d2e; }\n"
    "    .sim-state.sim      { color: #b07000; }\n"
    "    .sim-state.disabled { color: #888; font-style: italic; }\n"
    "    .sim-row select {\n"
    "      padding: 0.35em 0.4em; font: inherit;\n"
    "      border: 1px solid #ccc; border-radius:4px; background:white;\n"
    "    }\n"
    "    .sim-row select:disabled { opacity: 0.5; cursor: not-allowed; }\n"
    "\n"
    "    /* Units row */\n"
    "    .units-row { display:flex; gap:0.6em; align-items:center;\n"
    "                 margin: 0.4em 0; flex-wrap: wrap; }\n"
    "    .units-row label.lbl { flex: 0 0 8em; }\n"
    "    .units-row select {\n"
    "      padding: 0.35em 0.5em; font: inherit;\n"
    "      border: 1px solid #ccc; border-radius:4px; background:white;\n"
    "    }\n"
    "\n"
    "    /* Toggle switch (CSS-only) */\n"
    "    .toggle { position:relative; display:inline-block; width:54px; height:28px; }\n"
    "    .toggle input { opacity:0; width:0; height:0; }\n"
    "    .slider { position:absolute; cursor:pointer; inset:0;\n"
    "              background:#ccc; border-radius:28px; transition:.2s; }\n"
    "    .slider:before { position:absolute; content:\"\";\n"
    "                     height:22px; width:22px; left:3px; bottom:3px;\n"
    "                     background:white; border-radius:50%; transition:.2s; }\n"
    "    .toggle input:checked + .slider { background: #f5a623; }\n"
    "    .toggle input:checked + .slider:before { transform: translateX(26px); }\n"
    "    .toggle input:disabled + .slider { opacity: 0.5; cursor: not-allowed; }\n"
    "\n"
    "    .feedback { font-size: 0.85em; color:#666; min-height:1.2em; margin-top:0.6em; }\n"
    "    button.danger { background:#d4332a; color:white; border:none;\n"
    "                    padding: 0.5em 1em; border-radius:4px; cursor:pointer;\n"
    "                    font: inherit; }\n"
    "    button.danger:hover { background:#a82822; }\n"
    "\n"
    "    /* Hostname section */\n"
    "    .host-row { display:flex; gap:0.5em; align-items:center;\n"
    "                flex-wrap:wrap; margin: 0.4em 0; }\n"
    "    .host-row input[type=text] {\n"
    "      flex: 1; min-width: 12em;\n"
    "      padding: 0.4em 0.5em; font: inherit;\n"
    "      border: 1px solid #ccc; border-radius:4px;\n"
    "    }\n"
    "    .host-row button {\n"
    "      padding: 0.45em 0.9em; font: inherit; cursor:pointer;\n"
    "      background:#0366d6; color:white; border:none; border-radius:4px;\n"
    "    }\n"
    "    .host-row button:hover { background:#024891; }\n"
    "    .host-row button.secondary { background:#999; }\n"
    "    .host-row button.secondary:hover { background:#666; }\n"
    "    .host-current { font-weight: 600; }\n"
    "    .host-current a { color:#0366d6; }\n"
    "\n"
    "    /* Display settings */\n"
    "    .disp-row { display:flex; gap:0.6em; align-items:center;\n"
    "                margin: 0.4em 0; flex-wrap:wrap; }\n"
    "    .disp-row label { flex: 0 0 8em; }\n"
    "    /* The toggle is also a <label>, but we don't want the prose-label\n"
    "       width applied to it — otherwise the slider knob's 26px translate\n"
    "       leaves it sitting in the middle of an 8em-wide track. */\n"
    "    .disp-row .toggle { flex: 0 0 54px; }\n"
    "    .disp-row input[type=range] { flex: 1; min-width: 12em; }\n"
    "    .disp-row input[type=number] {\n"
    "      width: 6em; padding: 0.3em 0.4em; font: inherit;\n"
    "      border: 1px solid #ccc; border-radius:4px;\n"
    "    }\n"
    "    .disp-row .readout { width: 3em; font-variant-numeric: tabular-nums; }\n"
    "    .disp-actions { margin-top: 0.6em; }\n"
    "    .disp-actions button {\n"
    "      padding: 0.45em 0.9em; font: inherit; cursor:pointer;\n"
    "      background:#0366d6; color:white; border:none; border-radius:4px;\n"
    "    }\n"
    "    .disp-actions button:hover { background:#024891; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <h1 id=\"page-title\">cores3-hydro</h1>\n"
    "  <p class=\"muted\">Hydroponic monitor on M5Stack CoreS3. Firmware " FW_VERSION ".</p>\n"
    "\n"
    "  <h2>Live data</h2>\n"
    "  <ul>\n"
    "    <li><a href=\"/sensors\">/sensors</a> &mdash; latest readings (JSON)</li>\n"
    "    <li><a href=\"/status\">/status</a> &mdash; system health (JSON)</li>\n"
    "    <li><a href=\"/snapshot\">/snapshot</a> &mdash; current camera frame (JPEG)</li>\n"
    "  </ul>\n"
    "\n"
    "  <h2>Temperature units</h2>\n"
    "  <p class=\"muted\">Both the display and the <code>/sensors</code> JSON\n"
    "     emit temperatures in the unit you pick here. The response also\n"
    "     includes a <code>temperature_units</code> field so any client\n"
    "     knows which scale the numbers are on.</p>\n"
    "  <div class=\"units-row\">\n"
    "    <label class=\"lbl\" for=\"units-temp\">Temperature</label>\n"
    "    <select id=\"units-temp\">\n"
    "      <option value=\"fahrenheit\">Fahrenheit (°F)</option>\n"
    "      <option value=\"celsius\">Celsius (°C)</option>\n"
    "    </select>\n"
    "  </div>\n"
    "  <div id=\"units-feedback\" class=\"feedback\"></div>\n"
    "\n"
    "  <h2>Sensor mode</h2>\n"
    "  <p class=\"muted\">Each sensor can run in one of three modes:\n"
    "     <strong>Real</strong> reads the hardware,\n"
    "     <strong>Simulated</strong> feeds the agent random plausible values\n"
    "     (useful for development, demos, or quarantining a misbehaving\n"
    "     sensor), and <strong>Disabled</strong> turns the sensor off entirely\n"
    "     &mdash; the value reads as <code>null</code> with\n"
    "     <code>status: \"disabled\"</code>, signaling that the absence is\n"
    "     intentional (e.g. growing microgreens with no water tank).\n"
    "     Persists across reboots.</p>\n"
    "\n"
    "  <div id=\"sim-air\"   class=\"sim-row\"></div>\n"
    "  <div id=\"sim-water\" class=\"sim-row\"></div>\n"
    "  <div id=\"sim-light\" class=\"sim-row\"></div>\n"
    "  <div id=\"sim-feedback\" class=\"feedback\"></div>\n"
    "\n"
    "  <h2>Device name</h2>\n"
    "  <p class=\"muted\">This is the mDNS name other devices use to find this\n"
    "     unit on the local network. Useful when you have more than one. The\n"
    "     default includes the last 4 hex chars of the MAC so each unit is\n"
    "     uniquely discoverable out of the box.</p>\n"
    "\n"
    "  <p>Current: <span id=\"host-current\" class=\"host-current\">&hellip;</span></p>\n"
    "\n"
    "  <div class=\"host-row\">\n"
    "    <input id=\"host-input\" type=\"text\" maxlength=\"63\"\n"
    "           placeholder=\"e.g. greenhouse-1\"\n"
    "           autocapitalize=\"off\" autocomplete=\"off\" spellcheck=\"false\">\n"
    "    <button onclick=\"saveHost()\">Save</button>\n"
    "    <button class=\"secondary\" onclick=\"resetHost()\">Reset to default</button>\n"
    "  </div>\n"
    "  <p class=\"muted\" style=\"font-size:0.85em\">\n"
    "    Allowed: lowercase a&ndash;z, digits, hyphen. 1&ndash;63 characters.\n"
    "    No spaces, no leading/trailing hyphen.<br>\n"
    "    After saving, other devices' DNS caches may take ~2 minutes to\n"
    "    forget the old name. The new URL will be shown above.\n"
    "  </p>\n"
    "  <div id=\"host-feedback\" class=\"feedback\"></div>\n"
    "\n"
    "  <h2>Display</h2>\n"
    "  <p class=\"muted\">The screen is otherwise on 24/7 and the layout is\n"
    "     fully static. To extend backlight life and avoid LCD image\n"
    "     persistence, the display auto-dims after a short idle and turns\n"
    "     the backlight off after a longer idle. Touch the screen to wake.\n"
    "     Set either timeout to <code>0</code> to disable that step.</p>\n"
    "\n"
    "  <div class=\"disp-row\">\n"
    "    <label for=\"disp-bright\">Brightness</label>\n"
    "    <input id=\"disp-bright\" type=\"range\" min=\"0\" max=\"255\" step=\"1\">\n"
    "    <span id=\"disp-bright-out\" class=\"readout muted\">&hellip;</span>\n"
    "  </div>\n"
    "  <div class=\"disp-row\">\n"
    "    <label for=\"disp-dim\">Dim after (s)</label>\n"
    "    <input id=\"disp-dim\" type=\"number\" min=\"0\" max=\"86400\" step=\"1\">\n"
    "    <span class=\"muted\" style=\"font-size:0.85em\">0 disables dim</span>\n"
    "  </div>\n"
    "  <div class=\"disp-row\">\n"
    "    <label for=\"disp-sleep\">Sleep after (s)</label>\n"
    "    <input id=\"disp-sleep\" type=\"number\" min=\"0\" max=\"86400\" step=\"1\">\n"
    "    <span class=\"muted\" style=\"font-size:0.85em\">0 disables sleep</span>\n"
    "  </div>\n"
    "  <div class=\"disp-row\">\n"
    "    <label for=\"disp-flip\">Mounted upside down</label>\n"
    "    <label class=\"toggle\">\n"
    "      <input id=\"disp-flip\" type=\"checkbox\">\n"
    "      <span class=\"slider\"></span>\n"
    "    </label>\n"
    "    <span class=\"muted\" style=\"font-size:0.85em\">\n"
    "      Flips display + camera 180&deg; so the Grove cables hang down.</span>\n"
    "  </div>\n"
    "  <div class=\"disp-actions\">\n"
    "    <button onclick=\"saveDisplay()\">Save</button>\n"
    "  </div>\n"
    "  <div id=\"disp-feedback\" class=\"feedback\"></div>\n"
    "\n"
    "  <h2>Admin</h2>\n"
    "  <p>\n"
    "    <a href=\"/ota\">Firmware update</a>\n"
    "    <span class=\"muted\">&nbsp; &mdash; upload a new <code>.bin</code> from your browser.</span>\n"
    "  </p>\n"
    "  <p>\n"
    "    <button class=\"danger\" onclick=\"resetWifi()\">Reconfigure WiFi</button>\n"
    "    <span class=\"muted\">&nbsp; &mdash; wipes saved Wi-Fi and reboots into the setup AP.</span>\n"
    "  </p>\n"
    "\n"
    "  <script>\n"
    "  const SENSORS = [\n"
    "    {id:'air',   label:'Air',   note:'DHT20 on Port A &mdash; air temp + humidity'},\n"
    "    {id:'water', label:'Water', note:'DS18B20 on Port B &mdash; water temp'},\n"
    "    {id:'light', label:'Light', note:'LTR-553ALS (internal) &mdash; ambient light'}\n"
    "  ];\n"
    "  const MODES = [\n"
    "    {value:'real',      pretty:'real hardware', cls:'real'},\n"
    "    {value:'simulated', pretty:'SIMULATED',     cls:'sim'},\n"
    "    {value:'disabled',  pretty:'disabled',      cls:'disabled'}\n"
    "  ];\n"
    "  const fb = document.getElementById('sim-feedback');\n"
    "\n"
    "  function render(state) {\n"
    "    SENSORS.forEach(s => {\n"
    "      const mode = state[s.id] || 'real';\n"
    "      const m = MODES.find(x => x.value === mode) || MODES[0];\n"
    "      const row = document.getElementById('sim-' + s.id);\n"
    "      let opts = '';\n"
    "      MODES.forEach(x => {\n"
    "        const sel = x.value === mode ? ' selected' : '';\n"
    "        opts += '<option value=\"'+x.value+'\"'+sel+'>'+x.pretty+'</option>';\n"
    "      });\n"
    "      row.innerHTML =\n"
    "        '<span class=\"sim-name\">' + s.label + '</span>' +\n"
    "        '<span class=\"sim-state ' + m.cls + '\">' + m.pretty +\n"
    "          ' &middot; <span class=\"muted\">'+s.note+'</span></span>' +\n"
    "        '<select data-id=\"'+s.id+'\">' + opts + '</select>';\n"
    "      row.querySelector('select').addEventListener('change', onModeChange);\n"
    "    });\n"
    "  }\n"
    "\n"
    "  async function loadState() {\n"
    "    try {\n"
    "      const r = await fetch('/sim');\n"
    "      render(await r.json());\n"
    "    } catch(e) { fb.textContent = 'Could not load: ' + e; }\n"
    "  }\n"
    "\n"
    "  async function onModeChange(ev) {\n"
    "    const id = ev.target.dataset.id;\n"
    "    const value = ev.target.value;\n"
    "    const prev  = ev.target.dataset.prev || 'real';\n"
    "    ev.target.disabled = true;\n"
    "    fb.textContent = 'Setting ' + id + ' = ' + value + '...';\n"
    "    try {\n"
    "      const r = await fetch('/sim?' + id + '=' + value, {method:'POST'});\n"
    "      if (!r.ok) throw new Error(r.status);\n"
    "      const state = await r.json();\n"
    "      render(state);\n"
    "      fb.textContent = id + ' is now ' + state[id];\n"
    "    } catch(e) {\n"
    "      fb.textContent = 'Failed: ' + e;\n"
    "      ev.target.value = prev;\n"
    "      ev.target.disabled = false;\n"
    "    }\n"
    "  }\n"
    "\n"
    "  /* ----- temperature units ----- */\n"
    "  const unitsFb     = document.getElementById('units-feedback');\n"
    "  const unitsSelect = document.getElementById('units-temp');\n"
    "  unitsSelect.addEventListener('change', onUnitsChange);\n"
    "\n"
    "  async function loadUnits() {\n"
    "    try {\n"
    "      const r = await fetch('/units');\n"
    "      const s = await r.json();\n"
    "      unitsSelect.value = s.temperature_units || 'fahrenheit';\n"
    "    } catch(e) { unitsFb.textContent = 'Could not load units: ' + e; }\n"
    "  }\n"
    "\n"
    "  async function onUnitsChange() {\n"
    "    const v = unitsSelect.value;\n"
    "    unitsSelect.disabled = true;\n"
    "    unitsFb.textContent = 'Setting units = ' + v + '...';\n"
    "    try {\n"
    "      const r = await fetch('/units?temperature_units=' +\n"
    "                            encodeURIComponent(v), {method:'POST'});\n"
    "      if (!r.ok) throw new Error(r.status);\n"
    "      const s = await r.json();\n"
    "      unitsSelect.value = s.temperature_units;\n"
    "      unitsFb.textContent = 'Units saved: ' + s.temperature_units;\n"
    "    } catch(e) {\n"
    "      unitsFb.textContent = 'Failed: ' + e;\n"
    "    } finally {\n"
    "      unitsSelect.disabled = false;\n"
    "    }\n"
    "  }\n"
    "\n"
    "  async function resetWifi() {\n"
    "    if (!confirm('Wipe Wi-Fi credentials and reboot into WiFi setup AP mode?')) return;\n"
    "    try {\n"
    "      await fetch('/wifi/reset', {method:'POST'});\n"
    "      fb.textContent = 'Device is rebooting...';\n"
    "    } catch(e) { fb.textContent = 'Failed: ' + e; }\n"
    "  }\n"
    "\n"
    "  /* ----- hostname ----- */\n"
    "  const hostFb      = document.getElementById('host-feedback');\n"
    "  const hostCurrent = document.getElementById('host-current');\n"
    "  const hostInput   = document.getElementById('host-input');\n"
    "\n"
    "  function renderHost(state) {\n"
    "    const url = 'http://' + state.current + '.local/';\n"
    "    hostCurrent.innerHTML = state.current +\n"
    "      ' &middot; <a href=\"' + url + '\">' + url + '</a>';\n"
    "    hostInput.value = (state.current === state.default) ? '' : state.current;\n"
    "    hostInput.placeholder = 'e.g. greenhouse-1 (default: ' + state.default + ')';\n"
    "    /* Reflect the live name in the page heading and browser tab. */\n"
    "    document.getElementById('page-title').textContent = state.current;\n"
    "    document.title = state.current;\n"
    "  }\n"
    "\n"
    "  async function loadHost() {\n"
    "    try {\n"
    "      const r = await fetch('/hostname');\n"
    "      renderHost(await r.json());\n"
    "    } catch(e) { hostFb.textContent = 'Could not load hostname: ' + e; }\n"
    "  }\n"
    "\n"
    "  async function postHost(name) {\n"
    "    hostFb.textContent = 'Saving...';\n"
    "    try {\n"
    "      const r = await fetch('/hostname?name=' + encodeURIComponent(name),\n"
    "                            {method:'POST'});\n"
    "      if (!r.ok) {\n"
    "        const msg = await r.text();\n"
    "        hostFb.textContent = 'Rejected: ' + msg;\n"
    "        return;\n"
    "      }\n"
    "      const state = await r.json();\n"
    "      renderHost(state);\n"
    "      const url = 'http://' + state.current + '.local/';\n"
    "      hostFb.innerHTML = 'Saved. New URL: <a href=\"' + url + '\">' + url +\n"
    "        '</a> (your browser may need a moment to forget the old name).';\n"
    "    } catch(e) { hostFb.textContent = 'Failed: ' + e; }\n"
    "  }\n"
    "\n"
    "  function saveHost() {\n"
    "    const v = hostInput.value.trim().toLowerCase();\n"
    "    if (!v) { hostFb.textContent = 'Type a name, or use Reset to default.'; return; }\n"
    "    postHost(v);\n"
    "  }\n"
    "\n"
    "  function resetHost() {\n"
    "    if (!confirm('Reset device name to the per-MAC default?')) return;\n"
    "    postHost('');\n"
    "  }\n"
    "\n"
    "  /* ----- display settings ----- */\n"
    "  const dispFb     = document.getElementById('disp-feedback');\n"
    "  const dispBright = document.getElementById('disp-bright');\n"
    "  const dispOut    = document.getElementById('disp-bright-out');\n"
    "  const dispDim    = document.getElementById('disp-dim');\n"
    "  const dispSleep  = document.getElementById('disp-sleep');\n"
    "  const dispFlip   = document.getElementById('disp-flip');\n"
    "\n"
    "  dispBright.addEventListener('input', () => { dispOut.textContent = dispBright.value; });\n"
    "\n"
    "  function renderDisplay(state) {\n"
    "    dispBright.value = state.brightness;\n"
    "    dispOut.textContent = state.brightness;\n"
    "    dispDim.value   = Math.round(state.dim_ms   / 1000);\n"
    "    dispSleep.value = Math.round(state.sleep_ms / 1000);\n"
    "    dispFlip.checked = !!state.flipped;\n"
    "  }\n"
    "\n"
    "  async function loadDisplay() {\n"
    "    try {\n"
    "      const r = await fetch('/display');\n"
    "      renderDisplay(await r.json());\n"
    "    } catch(e) { dispFb.textContent = 'Could not load display settings: ' + e; }\n"
    "  }\n"
    "\n"
    "  async function saveDisplay() {\n"
    "    const b  = Math.max(0, Math.min(255, parseInt(dispBright.value, 10) || 0));\n"
    "    const ds = Math.max(0, parseInt(dispDim.value,   10) || 0);\n"
    "    const ss = Math.max(0, parseInt(dispSleep.value, 10) || 0);\n"
    "    const fl = dispFlip.checked ? 'on' : 'off';\n"
    "    const qs = 'brightness=' + b + '&dim_ms=' + (ds*1000) +\n"
    "               '&sleep_ms=' + (ss*1000) + '&flipped=' + fl;\n"
    "    dispFb.textContent = 'Saving...';\n"
    "    try {\n"
    "      const r = await fetch('/display?' + qs, {method:'POST'});\n"
    "      if (!r.ok) {\n"
    "        const msg = await r.text();\n"
    "        dispFb.textContent = 'Rejected: ' + msg;\n"
    "        return;\n"
    "      }\n"
    "      renderDisplay(await r.json());\n"
    "      dispFb.textContent = 'Saved.';\n"
    "    } catch(e) { dispFb.textContent = 'Failed: ' + e; }\n"
    "  }\n"
    "\n"
    "  loadState();\n"
    "  loadUnits();\n"
    "  loadHost();\n"
    "  loadDisplay();\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";
  s_server.send(200, "text/html", kHome);
}

// ---------------------------------------------------------------------------
// /sim — per-sensor mode management.
//
//   GET  /sim                                          → JSON of current modes
//   POST /sim?air=real&water=disabled&light=simulated  → set one or more modes
//
// Accepted values for each mode (case-insensitive):
//   real                       → REAL  — hardware reads
//   sim, simulated             → SIMULATED — random values from sim_*()
//   disabled, off, none        → DISABLED — value reads as null on /sensors
//
// All three params are optional; only listed sensors are changed. Unknown
// values return 400. The change is persisted to NVS before responding so a
// power cycle preserves it. The endpoint name and key set are kept for
// backwards compat with the pre-v0.7.0 boolean grammar — but the boolean
// shorthand (on/off/1/0/true/false/auto) is no longer accepted because
// "off" was ambiguous with the new DISABLED state.
// ---------------------------------------------------------------------------
static int parse_sim_value(const String &v) {
  // Kept as a 0/1 boolean parser specifically for /display?flipped=...
  // which never grew the DISABLED state. /sim uses parse_sensor_mode below.
  String s = v;
  s.toLowerCase();
  if (s == "on"  || s == "true"  || s == "1" || s == "sim")  return 1;
  if (s == "off" || s == "false" || s == "0" || s == "auto") return 0;
  return -1;   // invalid
}

static int parse_sensor_mode(const String &v) {
  String s = v;
  s.toLowerCase();
  if (s == "real")                                return (int)SensorMode::REAL;
  if (s == "sim" || s == "simulated")             return (int)SensorMode::SIMULATED;
  if (s == "disabled" || s == "off" || s == "none") return (int)SensorMode::OFF;
  return -1;
}

static void emit_sim_state(int code) {
  JsonDocument doc;
  doc["air"]   = sensor_mode_label(sensor_mode_from(g_state.air_mode.load()));
  doc["water"] = sensor_mode_label(sensor_mode_from(g_state.water_mode.load()));
  doc["light"] = sensor_mode_label(sensor_mode_from(g_state.light_mode.load()));
  String out;
  serializeJson(doc, out);
  s_server.send(code, "application/json", out);
}

static void handle_sim() {
  if (s_server.method() == HTTP_GET) {
    emit_sim_state(200);
    return;
  }
  if (s_server.method() != HTTP_POST) {
    s_server.send(405, "text/plain", "use GET or POST");
    return;
  }

  bool changed_air = false, changed_water = false, changed_light = false;

  auto try_apply = [&](const char *name, std::atomic<uint8_t> &field,
                       bool &changed) -> int {
    if (!s_server.hasArg(name)) return 0;
    int v = parse_sensor_mode(s_server.arg(name));
    if (v < 0) return -1;
    uint8_t desired = (uint8_t)v;
    if (field.load() != desired) {
      field.store(desired);
      changed = true;
    }
    return 0;
  };

  if (try_apply("air",   g_state.air_mode,   changed_air)   < 0) {
    s_server.send(400, "text/plain",
                  "air: use real | simulated | disabled");
    return;
  }
  if (try_apply("water", g_state.water_mode, changed_water) < 0) {
    s_server.send(400, "text/plain",
                  "water: use real | simulated | disabled");
    return;
  }
  if (try_apply("light", g_state.light_mode, changed_light) < 0) {
    s_server.send(400, "text/plain",
                  "light: use real | simulated | disabled");
    return;
  }

  if (changed_air)   sim_state_save_air();
  if (changed_water) sim_state_save_water();
  if (changed_light) sim_state_save_light();

  emit_sim_state(200);
}

// ---------------------------------------------------------------------------
// /units — user-selected temperature unit (C/F).
//
//   GET  /units                              → {"temperature_units": "..."}
//   POST /units?temperature_units=celsius    → persist + echo
//   POST /units?temperature_units=fahrenheit → persist + echo
//
// Accepts case-insensitive "celsius"/"c" and "fahrenheit"/"f". The /sensors
// response always tells the agent which unit it's reading via the same
// `temperature_units` field — option B from the v0.7.0 design: the device
// converts on egress so OpenClaw doesn't have to.
// ---------------------------------------------------------------------------
static int parse_temp_unit(const String &v) {
  String s = v;
  s.toLowerCase();
  if (s == "celsius"    || s == "c") return (int)TempUnit::CELSIUS;
  if (s == "fahrenheit" || s == "f") return (int)TempUnit::FAHRENHEIT;
  return -1;
}

static void emit_units_state(int code) {
  JsonDocument doc;
  doc["temperature_units"] = temp_unit_label();
  String out;
  serializeJson(doc, out);
  s_server.send(code, "application/json", out);
}

static void handle_units() {
  if (s_server.method() == HTTP_GET) {
    emit_units_state(200);
    return;
  }
  if (s_server.method() != HTTP_POST) {
    s_server.send(405, "text/plain", "use GET or POST");
    return;
  }

  if (!s_server.hasArg("temperature_units")) {
    s_server.send(400, "text/plain",
                  "missing temperature_units parameter (celsius|fahrenheit)");
    return;
  }
  int v = parse_temp_unit(s_server.arg("temperature_units"));
  if (v < 0) {
    s_server.send(400, "text/plain",
                  "temperature_units must be celsius|c|fahrenheit|f");
    return;
  }
  temp_unit_set((TempUnit)v);
  emit_units_state(200);
}

// ---------------------------------------------------------------------------
// /hostname — runtime mDNS hostname management.
//
//   GET  /hostname                 → JSON of current name, default, MAC, URL
//   POST /hostname?name=<label>    → set new name (validated, persisted)
//   POST /hostname?name=           → reset to per-MAC default
//
// On successful POST: persists to NVS, re-announces mDNS, returns the same
// JSON shape as GET. On invalid input: 400 with a short text message.
// ---------------------------------------------------------------------------
static void emit_hostname_state(int code) {
  JsonDocument doc;
  String current(device_hostname());
  doc["current"] = current;
  doc["default"] = device_hostname_default();
  doc["mac"]     = device_mac();
  doc["url"]     = String("http://") + current + ".local/";
  String out;
  serializeJson(doc, out);
  s_server.send(code, "application/json", out);
}

static void handle_hostname() {
  if (s_server.method() == HTTP_GET) {
    emit_hostname_state(200);
    return;
  }
  if (s_server.method() != HTTP_POST) {
    s_server.send(405, "text/plain", "use GET or POST");
    return;
  }

  if (!s_server.hasArg("name")) {
    s_server.send(400, "text/plain", "missing 'name' parameter");
    return;
  }

  String name = s_server.arg("name");
  // Empty value clears the override (resets to default). Non-empty values
  // are normalized + validated inside device_hostname_set(), which returns
  // false on either bad-input or NVS-write failure. Disambiguate with
  // device_hostname_validate so we can echo the specific error.
  if (name.length() > 0) {
    String normalized = name;
    normalized.trim();
    normalized.toLowerCase();
    const char *err = device_hostname_validate(normalized.c_str());
    if (err) {
      s_server.send(400, "text/plain", err);
      return;
    }
  }

  if (!device_hostname_set(name.c_str())) {
    s_server.send(500, "text/plain", "could not save hostname");
    return;
  }

  // mDNS re-announce happens here, not inside device_hostname_set, so the
  // device_name module stays free of network dependencies.
  net_apply_hostname_change();

  emit_hostname_state(200);
}

// ---------------------------------------------------------------------------
// /display — display brightness and idle-timeout management.
//
//   GET  /display
//     → {"brightness":<0..255>,"dim_ms":<u32>,"sleep_ms":<u32>}
//   POST /display?brightness=<0..255>&dim_ms=<u32>&sleep_ms=<u32>
//     → same JSON shape, reflecting the new state.
//
// All three POST params are optional; only listed fields are changed.
// Timeouts of 0 disable that transition (always-on, or no sleep). Values
// are clamped to 24 h on the upper end to keep the math sane. The change
// is persisted to NVS before responding so a power cycle preserves it.
// ---------------------------------------------------------------------------
static const uint32_t MAX_TIMEOUT_MS = 24UL * 60UL * 60UL * 1000UL;  // 24 h

static void emit_display_state(int code) {
  JsonDocument doc;
  doc["brightness"] = g_state.display_brightness.load();
  doc["dim_ms"]     = g_state.display_dim_ms.load();
  doc["sleep_ms"]   = g_state.display_sleep_ms.load();
  doc["flipped"]    = g_state.display_flipped.load();
  String out;
  serializeJson(doc, out);
  s_server.send(code, "application/json", out);
}

static void handle_display() {
  if (s_server.method() == HTTP_GET) {
    emit_display_state(200);
    return;
  }
  if (s_server.method() != HTTP_POST) {
    s_server.send(405, "text/plain", "use GET or POST");
    return;
  }

  bool changed_bright = false, changed_dim = false, changed_sleep = false,
       changed_flip   = false;

  if (s_server.hasArg("flipped")) {
    int v = parse_sim_value(s_server.arg("flipped"));   // same on/off/1/0/true/false grammar
    if (v < 0) {
      s_server.send(400, "text/plain", "flipped must be on/off/true/false/1/0");
      return;
    }
    bool desired = (v == 1);
    if (g_state.display_flipped.load() != desired) {
      g_state.display_flipped.store(desired);
      changed_flip = true;
    }
  }

  if (s_server.hasArg("brightness")) {
    long v = s_server.arg("brightness").toInt();
    if (v < 0 || v > 255) {
      s_server.send(400, "text/plain", "brightness must be 0..255");
      return;
    }
    uint8_t nv = (uint8_t)v;
    if (g_state.display_brightness.load() != nv) {
      g_state.display_brightness.store(nv);
      changed_bright = true;
    }
  }

  auto try_apply_timeout = [&](const char *name, std::atomic<uint32_t> &field,
                               bool &changed) -> int {
    if (!s_server.hasArg(name)) return 0;
    long v = s_server.arg(name).toInt();
    if (v < 0 || (uint32_t)v > MAX_TIMEOUT_MS) return -1;
    uint32_t nv = (uint32_t)v;
    if (field.load() != nv) {
      field.store(nv);
      changed = true;
    }
    return 0;
  };

  if (try_apply_timeout("dim_ms", g_state.display_dim_ms, changed_dim) < 0) {
    s_server.send(400, "text/plain", "dim_ms out of range (0..86400000)");
    return;
  }
  if (try_apply_timeout("sleep_ms", g_state.display_sleep_ms, changed_sleep) < 0) {
    s_server.send(400, "text/plain", "sleep_ms out of range (0..86400000)");
    return;
  }

  if (changed_bright) display_settings_save_brightness();
  if (changed_dim)    display_settings_save_dim_ms();
  if (changed_sleep)  display_settings_save_sleep_ms();
  if (changed_flip) {
    display_settings_save_flip();
    // Apply both halves of the orientation flip immediately. Display
    // rotation 3 = landscape, 180° from rotation 1. The camera sensor
    // gets matching hmirror+vflip so /snapshot lines up with the screen.
    M5.Display.setRotation(g_state.display_flipped.load() ? 3 : 1);
    camera_set_flip(g_state.display_flipped.load());
  }

  // Treat any change as user activity — they're at the device. Without
  // this, lowering the dim timeout while idle would dim the screen mid-edit.
  if (changed_bright || changed_dim || changed_sleep || changed_flip) {
    g_state.last_touch_ms.store(millis());
  }

  emit_display_state(200);
}

// ---------------------------------------------------------------------------
// POST /wifi/reset — wipe stored WiFi creds and reboot into the setup AP.
//
// No auth (we're a LAN-trusted device). Useful for the agent to recover the
// box if it ever ends up on a wrong network, or for a remote re-setup
// without physical access.
// ---------------------------------------------------------------------------
static void handle_wifi_reset() {
  if (s_server.method() != HTTP_POST) {
    s_server.send(405, "text/plain", "use POST");
    return;
  }
  Serial.println("[http] /wifi/reset received; rebooting to setup");
  s_server.send(200, "text/plain",
                "Wi-Fi credentials cleared. Device will reboot in 2s and "
                "open the WiFi setup AP (cores3-hydro-setup-<mac>) at "
                "http://192.168.4.1/.");
  delay(500);
  wifi_creds_clear();
  delay(1500);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// 404
// ---------------------------------------------------------------------------
static void handle_not_found() {
  s_server.send(404, "text/plain", "not found");
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void http_server_begin() {
  s_server.on("/",           handle_root);
  s_server.on("/sensors",    handle_sensors);
  s_server.on("/status",     handle_status);
  s_server.on("/snapshot",   handle_snapshot);
  s_server.on("/sim",        handle_sim);
  s_server.on("/units",      handle_units);
  s_server.on("/hostname",   handle_hostname);
  s_server.on("/display",    handle_display);
  s_server.on("/wifi/reset", handle_wifi_reset);

  // OTA endpoints (/ota, /ota/upload). Owned by ota.cpp — it also opts in
  // to the Content-Length request header via collectHeaders(), which is
  // why this must happen on our s_server instance and not a separate one.
  ota_register(s_server);

  s_server.onNotFound(handle_not_found);
  s_server.begin();
}

void http_server_loop() {
  s_server.handleClient();
}
