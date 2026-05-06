// ota.cpp — HTTP-based OTA firmware update with PSRAM-buffered upload.
//
// Endpoints:
//   GET  /ota          — file-picker HTML with progress UI
//   POST /ota/upload   — multipart upload of a raw .bin firmware image
//
// Upload mechanics
// ----------------
// We use the WebServer's standard multipart-form-data upload hook. The
// alternative (raw POST + manual `client().read()` into a buffer) would
// require either an HTTP_RAW handler API the M5Stack arduino-esp32 fork
// doesn't expose, or trusting that WebServer leaves a 6 MB body unparsed
// on the stream — which it doesn't (it tries to slurp non-form bodies into
// `arg("plain")`). Multipart with the onUpload hook gives us chunk-at-a-time
// callbacks before the buffer is committed to `arg("plain")`, which is
// exactly the streaming primitive we need.
//
// Total firmware size isn't in the multipart envelope but Content-Length is,
// so we register Content-Length via collectHeaders() and pre-allocate the
// PSRAM buffer at UPLOAD_FILE_START. A few bytes of multipart overhead are
// included in the allocation — acceptable, since we track actual received
// bytes separately and only Update.write() that count.
//
// State machine
// -------------
//   UPLOAD_FILE_START   →  validate Content-Length, set ota_in_progress,
//                          camera_stop(), ps_malloc, init Update library
//   UPLOAD_FILE_WRITE   →  memcpy chunk into PSRAM buffer
//   UPLOAD_FILE_END     →  Update.begin/write/end, free buffer; the matching
//                          completion handler then sends "rebooting" HTML
//                          and ESP.restart()s. ota_in_progress stays true
//                          (we're rebooting; clearing it is moot)
//   UPLOAD_FILE_ABORTED →  free buffer, clear flag, Update.abort()
//
// Any failure within the chunk callbacks records an error in s_error_msg.
// The completion handler reads that and returns 500 + JSON, leaving the
// running firmware untouched. ota_in_progress is cleared so sensors resume.

#include <Arduino.h>
#include <WebServer.h>
#include <Update.h>

#include "ota.h"
#include "sensors.h"
#include "camera.h"
#include "device_name.h"
#include "config.h"

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
// CoreS3 has 16 MB flash. The default ESP32 OTA partition layout reserves
// roughly 6.25 MB per app partition — we cap below that with a generous
// margin. If/when we customize partitions we can revisit.
static const size_t OTA_MAX_SIZE = 6UL * 1024 * 1024;

// ---------------------------------------------------------------------------
// Module state. The WebServer's onUpload + completion handlers run on the
// loop task (synchronous WebServer), so plain statics are safe — no atomics
// or mutexes needed here.
// ---------------------------------------------------------------------------
static WebServer *s_server         = nullptr;
static uint8_t   *s_buf            = nullptr;
static size_t     s_capacity       = 0;
static size_t     s_received       = 0;
static String     s_error_msg;       // empty = no error so far

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void ota_record_error(const String &msg) {
  Serial.printf("[ota] FAIL: %s\n", msg.c_str());
  if (s_error_msg.length() == 0) s_error_msg = msg;
  if (s_buf) {
    free(s_buf);
    s_buf = nullptr;
    s_capacity = 0;
  }
  s_received = 0;
  if (Update.isRunning() || Update.hasError()) {
    Update.abort();
  }
  g_state.ota_in_progress.store(false);
}

// ---------------------------------------------------------------------------
// GET /ota — picker page
// ---------------------------------------------------------------------------
static void handle_ota_index() {
  // The HTML lives as a static const char* so it's flash-resident and not
  // copied to RAM on each request. Inline CSS+JS for a single self-contained
  // page (matches the home-page convention in http_server.cpp).
  static const char *kPage =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <title>OTA · cores3-hydro</title>\n"
    "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "  <style>\n"
    "    body { font: 15px system-ui, sans-serif; max-width: 560px;\n"
    "           margin: 2em auto; padding: 0 1em; color:#222; }\n"
    "    h1 { margin: 0 0 0.5em; }\n"
    "    .warn { background:#fff8e1; border-left:3px solid #f5a623;\n"
    "            padding:0.6em 1em; margin: 1em 0; }\n"
    "    form { margin: 1.5em 0; padding: 1em; border: 1px solid #ddd;\n"
    "           border-radius: 6px; }\n"
    "    input[type=file] { font: inherit; }\n"
    "    button { padding: 0.5em 1em; font: inherit; cursor: pointer;\n"
    "             background:#0366d6; color:white; border:none; border-radius:4px; }\n"
    "    button:hover { background:#024891; }\n"
    "    button:disabled { background:#999; cursor: not-allowed; }\n"
    "    progress { width: 100%; height: 1.5em; }\n"
    "    .status { margin-top: 1em; min-height: 1.2em; color:#666; }\n"
    "    .status.err { color:#c00; }\n"
    "    a { color:#0366d6; }\n"
    "    code { background:#eee; padding:1px 4px; border-radius:3px; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <h1>Firmware update</h1>\n"
    "  <p>Current firmware: <code>" FW_VERSION "</code></p>\n"
    "  <p class=\"warn\">Sensor reads pause and the camera shuts down while the\n"
    "    firmware is buffered in PSRAM. The device reboots automatically once\n"
    "    the new image is verified. If anything goes wrong before flash is\n"
    "    written, the running firmware stays untouched.</p>\n"
    "  <form id=\"f\" enctype=\"multipart/form-data\" method=\"POST\" action=\"/ota/upload\">\n"
    "    <p><input id=\"file\" type=\"file\" name=\"firmware\" accept=\".bin\" required></p>\n"
    "    <p><button id=\"go\" type=\"submit\">Upload firmware</button></p>\n"
    "    <progress id=\"p\" value=\"0\" max=\"100\" hidden></progress>\n"
    "    <div id=\"s\" class=\"status\"></div>\n"
    "  </form>\n"
    "  <p><a href=\"/\">&larr; back to home</a></p>\n"
    "  <script>\n"
    "  const f  = document.getElementById('f');\n"
    "  const p  = document.getElementById('p');\n"
    "  const s  = document.getElementById('s');\n"
    "  const go = document.getElementById('go');\n"
    "  f.addEventListener('submit', e => {\n"
    "    e.preventDefault();\n"
    "    const fd = new FormData(f);\n"
    "    const xhr = new XMLHttpRequest();\n"
    "    xhr.upload.addEventListener('progress', ev => {\n"
    "      if (ev.lengthComputable) {\n"
    "        p.hidden = false;\n"
    "        p.max = ev.total;\n"
    "        p.value = ev.loaded;\n"
    "        s.className = 'status';\n"
    "        s.textContent = 'Uploading ' + Math.round(ev.loaded/ev.total*100) + '%...';\n"
    "      }\n"
    "    });\n"
    "    xhr.addEventListener('load', () => {\n"
    "      if (xhr.status >= 200 && xhr.status < 300) {\n"
    "        s.className = 'status';\n"
    "        s.innerHTML = 'Upload complete. Device is rebooting &mdash; ' +\n"
    "          'after about 10 seconds, <a href=\"/\">click here</a> to return home.';\n"
    "      } else {\n"
    "        s.className = 'status err';\n"
    "        s.textContent = 'Failed (' + xhr.status + '): ' + xhr.responseText;\n"
    "        go.disabled = false;\n"
    "      }\n"
    "    });\n"
    "    xhr.addEventListener('error', () => {\n"
    "      s.className = 'status err';\n"
    "      s.textContent = 'Connection error during upload.';\n"
    "      go.disabled = false;\n"
    "    });\n"
    "    go.disabled = true;\n"
    "    s.className = 'status';\n"
    "    s.textContent = 'Starting upload...';\n"
    "    xhr.open('POST', '/ota/upload');\n"
    "    xhr.send(fd);\n"
    "  });\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";
  s_server->send(200, "text/html", kPage);
}

// ---------------------------------------------------------------------------
// POST /ota/upload — completion handler.
//
// Called by WebServer after onUpload finishes (whether success or not). We
// inspect s_error_msg / s_received to decide what to send back. On success,
// reply 200 then ESP.restart() so the new image takes over.
// ---------------------------------------------------------------------------
static void handle_ota_upload_complete() {
  if (s_error_msg.length() > 0) {
    String body = "{\"ok\":false,\"error\":\"";
    // Tiny escape: only quotes need it for our error strings.
    String e = s_error_msg;
    e.replace("\"", "'");
    body += e;
    body += "\"}";
    s_server->send(500, "application/json", body);
    return;
  }
  // Success path: image written, partition switched. Send the page first so
  // the user's browser can render it before the reboot.
  static const char *kDone =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>OTA OK</title>"
    "<meta http-equiv=\"refresh\" content=\"15;url=/\">"
    "<style>body{font:15px system-ui,sans-serif;max-width:560px;"
    "margin:2em auto;padding:0 1em;}</style></head><body>"
    "<h1>Update OK</h1>"
    "<p>Device is rebooting. This page will redirect home in 15 seconds.</p>"
    "</body></html>";
  s_server->send(200, "text/html", kDone);

  Serial.println("[ota] flashed OK; rebooting in 2 s");
  delay(2000);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// POST /ota/upload — chunk handler (called repeatedly during upload).
// ---------------------------------------------------------------------------
static void handle_ota_upload_chunk() {
  HTTPUpload &up = s_server->upload();

  switch (up.status) {
  case UPLOAD_FILE_START: {
    s_error_msg = "";
    s_received  = 0;

    // Content-Length includes multipart boundary overhead, but it's a tight
    // upper bound — within a few hundred bytes — so we use it for both the
    // size sanity-cap AND the ps_malloc capacity. We track the real
    // firmware byte count in s_received and only Update.write() that.
    String cl_str = s_server->header("Content-Length");
    long cl = cl_str.length() ? cl_str.toInt() : 0;
    if (cl <= 0) {
      ota_record_error("missing or invalid Content-Length");
      return;
    }
    if ((size_t)cl > OTA_MAX_SIZE) {
      ota_record_error(String("upload too large (") + cl + " > " +
                       (uint32_t)OTA_MAX_SIZE + ")");
      return;
    }

    Serial.printf("[ota] upload start: filename=\"%s\" Content-Length=%ld\n",
                  up.filename.c_str(), cl);

    // Mark the device busy. Sensor tasks idle after their next wakeup; the
    // display task switches to the OTA screen on its next 1 s tick.
    g_state.ota_in_progress.store(true);

    // Free camera framebuffers (~1.2 MB of PSRAM) so the upload buffer fits.
    Serial.printf("[ota] PSRAM free before camera_stop: %u bytes\n",
                  (unsigned)ESP.getFreePsram());
    camera_stop();
    Serial.printf("[ota] PSRAM free after  camera_stop: %u bytes\n",
                  (unsigned)ESP.getFreePsram());

    s_buf = (uint8_t *)ps_malloc((size_t)cl);
    if (!s_buf) {
      ota_record_error(String("ps_malloc(") + cl + ") failed");
      return;
    }
    s_capacity = (size_t)cl;
    Serial.printf("[ota] allocated %u-byte PSRAM buffer\n", (unsigned)s_capacity);
    break;
  }

  case UPLOAD_FILE_WRITE: {
    if (s_error_msg.length() > 0 || !s_buf) return;
    if (s_received + up.currentSize > s_capacity) {
      ota_record_error("upload exceeded declared Content-Length");
      return;
    }
    memcpy(s_buf + s_received, up.buf, up.currentSize);
    s_received += up.currentSize;
    break;
  }

  case UPLOAD_FILE_END: {
    if (s_error_msg.length() > 0 || !s_buf) return;
    Serial.printf("[ota] upload complete: %u bytes received\n",
                  (unsigned)s_received);

    if (s_received == 0) {
      ota_record_error("zero-byte upload");
      return;
    }

    if (!Update.begin(s_received)) {
      ota_record_error(String("Update.begin failed: ") + Update.errorString());
      return;
    }
    size_t written = Update.write(s_buf, s_received);
    if (written != s_received) {
      ota_record_error(String("Update.write incomplete: ") + (uint32_t)written +
                       "/" + (uint32_t)s_received);
      return;
    }
    if (!Update.end(true)) {
      ota_record_error(String("Update.end failed: ") + Update.errorString());
      return;
    }

    // Free the buffer now — flash already has the bytes. We deliberately
    // leave ota_in_progress = true; the device is rebooting, and clearing
    // the flag would just race with that.
    free(s_buf);
    s_buf = nullptr;
    s_capacity = 0;
    Serial.println("[ota] flash write OK; awaiting completion handler");
    break;
  }

  case UPLOAD_FILE_ABORTED:
    ota_record_error("upload aborted by client");
    break;

  default:
    // No-op for unknown states.
    break;
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ota_register(WebServer &server) {
  s_server = &server;

  server.on("/ota",        HTTP_GET,  handle_ota_index);
  server.on("/ota/upload", HTTP_POST, handle_ota_upload_complete,
                                       handle_ota_upload_chunk);

  // The chunk handler reads Content-Length to size its PSRAM buffer.
  // WebServer doesn't expose request headers by default — we have to
  // opt in to the ones we need.
  static const char *kHeaders[] = {"Content-Length"};
  server.collectHeaders(kHeaders, sizeof(kHeaders) / sizeof(kHeaders[0]));
}
