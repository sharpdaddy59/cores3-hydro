// wifi_setup.cpp — QR-code WiFi setup with live camera preview.
//
// Flow:
//   1. Take over the display.
//   2. Continuously: capture an RGB565 frame from the camera, push it to
//      the display as a live preview, convert a copy to grayscale, hand the
//      grayscale to quirc, run quirc's identify+decode pipeline, check if
//      the result is a "WIFI:" payload.
//   3. On a valid WIFI: payload, save creds to NVS and return.
//   4. Abort: long-press the bottom-right corner.
//
// Display layout during setup:
//   Top 30 px:    black bar with "Wi-Fi Setup" + scan status text.
//   Middle 200px: live camera preview (320x200 strip out of 320x240 frame).
//   Bottom 10px:  black footer with abort hint.
//
// Memory: at VGA (640x480), quirc allocates a 307 KB grayscale image
// buffer and we allocate a 153 KB downsampled preview buffer — both in
// PSRAM. Plus a few KB of internal scratch in DRAM.

#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <esp_camera.h>

extern "C" {
#include "quirc.h"
}

#include "wifi_setup.h"
#include "config.h"

// ---------------------------------------------------------------------------
// NVS-backed credential storage
// ---------------------------------------------------------------------------
static const char *NVS_NS    = "wifi";
static const char *NVS_SSID  = "ssid";
static const char *NVS_PASS  = "pass";

bool wifi_creds_load(WifiCreds *out) {
  if (!out) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, /*readOnly=*/true)) return false;
  out->ssid     = prefs.getString(NVS_SSID, "");
  out->password = prefs.getString(NVS_PASS, "");
  prefs.end();
  return out->ssid.length() > 0;   // password may be empty for open networks
}

bool wifi_creds_save(const WifiCreds *creds) {
  if (!creds) return false;
  Preferences prefs;
  if (!prefs.begin(NVS_NS, /*readOnly=*/false)) return false;
  prefs.putString(NVS_SSID, creds->ssid);
  prefs.putString(NVS_PASS, creds->password);
  prefs.end();
  return true;
}

void wifi_creds_clear() {
  Preferences prefs;
  if (prefs.begin(NVS_NS, false)) {
    prefs.clear();
    prefs.end();
  }
}

// ---------------------------------------------------------------------------
// WIFI: QR payload parser
// ---------------------------------------------------------------------------
//
// Format (per ZXing / Wi-Fi Alliance spec):
//   WIFI:T:WPA;S:mynet;P:mypass;H:false;;
// Fields are semicolon-separated key:value pairs. Special chars escaped
// with backslash: \; \, \: \\
// We care about S (SSID) and P (password). T is informational; we let
// WiFi.begin() figure out the auth mode.
//
// Returns true on a recognizable payload with at least an SSID.
static bool parse_wifi_qr(const uint8_t *data, int len, WifiCreds *out) {
  if (!data || !out) return false;
  if (len < 6) return false;

  String s((const char *)data, len);
  if (!s.startsWith("WIFI:")) return false;
  s = s.substring(5);   // strip "WIFI:"

  String ssid, pass;
  bool in_key = true;
  String key, val;
  bool escape = false;

  for (int i = 0; i < (int)s.length(); ++i) {
    char c = s.charAt(i);
    if (escape) {
      // Append literally, terminate escape.
      (in_key ? key : val) += c;
      escape = false;
      continue;
    }
    if (c == '\\') { escape = true; continue; }

    if (in_key) {
      if (c == ':') { in_key = false; continue; }
      if (c == ';') {
        // Empty field separator (e.g., trailing ";;"). Reset.
        key = ""; val = "";
        continue;
      }
      key += c;
    } else {
      if (c == ';') {
        // End of value. Commit the key/value.
        if (key == "S")      ssid = val;
        else if (key == "P") pass = val;
        // T, H, and any others are intentionally ignored.
        key = ""; val = "";
        in_key = true;
      } else {
        val += c;
      }
    }
  }

  if (ssid.length() == 0) return false;
  out->ssid     = ssid;
  out->password = pass;
  return true;
}

// ---------------------------------------------------------------------------
// RGB565 → grayscale conversion for quirc input
// ---------------------------------------------------------------------------
//
// Camera outputs RGB565 in big-endian byte order (high byte first).
// Standard luminance: Y' = 0.299R + 0.587G + 0.114B. We approximate as
// (R + 2G + B) / 4 which is fast and good enough for QR scanning.
static void rgb565_to_grayscale(const uint8_t *rgb565,
                                uint8_t *gray,
                                int n_pixels) {
  for (int i = 0; i < n_pixels; ++i) {
    uint16_t px = ((uint16_t)rgb565[2*i] << 8) | rgb565[2*i + 1];
    uint8_t r5 = (px >> 11) & 0x1F;
    uint8_t g6 = (px >> 5)  & 0x3F;
    uint8_t b5 =  px        & 0x1F;
    // Scale to 8-bit (replicate top bits into LSBs).
    uint8_t r = (r5 << 3) | (r5 >> 2);
    uint8_t g = (g6 << 2) | (g6 >> 4);
    uint8_t b = (b5 << 3) | (b5 >> 2);
    gray[i] = (uint8_t)((r + 2*g + b) >> 2);
  }
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------
static void draw_overlay(const char *status_line, uint16_t status_color) {
  // Top bar: title
  M5.Display.fillRect(0, 0, 320, 30, TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 7);
  M5.Display.print("Wi-Fi Setup");
  // Bottom bar: status / abort hint
  M5.Display.fillRect(0, 230, 320, 10, TFT_BLACK);
  M5.Display.setTextColor(status_color, TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(5, 232);
  M5.Display.print(status_line);
}

static void draw_initial_screen() {
  // Brief splash with the one critical hint: don't try to scan a phone
  // screen. Phone subpixel matrix moires against the GC0308's Bayer grid
  // and produces undecodable images. A printed QR or one displayed on a
  // larger-pixel-pitch screen (laptop monitor, tablet) works fine.
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(60, 70);
  M5.Display.println("Wi-Fi Setup");
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
  M5.Display.setCursor(20, 130);
  M5.Display.println("Show a printed QR code");
  M5.Display.setCursor(20, 145);
  M5.Display.println("or a laptop monitor.");
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.setCursor(20, 175);
  M5.Display.println("(Phone screens don't scan well -");
  M5.Display.setCursor(20, 188);
  M5.Display.println("camera-vs-screen pixel moire.)");
  delay(2500);
}

// ---------------------------------------------------------------------------
// Abort gesture: long-press in the bottom-right 60x60 zone.
// ---------------------------------------------------------------------------
static bool check_abort_gesture(uint32_t &press_started_ms) {
  M5.update();
  auto t = M5.Touch.getDetail();
  bool in_zone = t.isPressed()
      && t.x >= 260 && t.x < 320
      && t.y >= 180 && t.y < 240;
  if (in_zone) {
    if (press_started_ms == 0) press_started_ms = millis();
    if (millis() - press_started_ms >= 2000) return true;   // 2s hold = abort
  } else {
    press_started_ms = 0;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------
bool wifi_setup_run(WifiCreds *out) {
  if (!out) return false;
  Serial.println("[setup] entering Wi-Fi QR setup mode");

  draw_initial_screen();

  // Camera warmup: discard ~30 frames (~2-3 seconds) so the auto-exposure,
  // white balance, and sensor gain control have time to converge on
  // the live scene. Without this, the first dozen scan attempts run
  // against frames where the exposure is still adapting from boot
  // defaults — which is why the live scan often fails to detect QRs
  // that are obviously decodable in /snapshot (taken much later when
  // the camera has long since stabilized).
  Serial.println("[setup] camera warmup...");
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_DARKGREY, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(40, 110);
  M5.Display.println("Warming up...");
  for (int i = 0; i < 30; ++i) {
    camera_fb_t *warmup_fb = esp_camera_fb_get();
    if (warmup_fb) esp_camera_fb_return(warmup_fb);
    delay(50);
  }
  Serial.println("[setup] camera warmup complete");

  // Allocate quirc with VGA-sized image buffer. quirc.c was patched to
  // route the image buffer through heap_caps PSRAM, so 307 KB lives in
  // PSRAM rather than DRAM (which would not fit).
  struct quirc *q = quirc_new();
  if (!q) {
    Serial.println("[setup] FATAL: quirc_new failed");
    return false;
  }
  if (quirc_resize(q, 640, 480) < 0) {
    Serial.println("[setup] FATAL: quirc_resize(640, 480) failed");
    quirc_destroy(q);
    return false;
  }
  Serial.printf("[setup] quirc ready at 640x480; PSRAM free=%u KB\n",
                (unsigned)(ESP.getFreePsram() / 1024));

  // Allocate preview buffer in PSRAM. We downsample+mirror the VGA frame
  // into this buffer for display each iteration.
  uint16_t *preview = (uint16_t *)ps_malloc(320 * 240 * sizeof(uint16_t));
  if (!preview) {
    Serial.println("[setup] FATAL: ps_malloc(preview) failed");
    quirc_destroy(q);
    return false;
  }

  uint32_t abort_press_ms      = 0;
  uint32_t frame_count         = 0;
  uint32_t last_status_ms      = 0;
  uint32_t total_ecc_failures  = 0;
  uint32_t total_codes_seen    = 0;

  for (;;) {
    if (check_abort_gesture(abort_press_ms)) {
      Serial.println("[setup] aborted by user");
      free(preview);
      quirc_destroy(q);
      return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      delay(50);
      continue;
    }

    // 1. Convert the full VGA frame to grayscale into quirc's image buffer.
    //    Original (un-mirrored) data — quirc can't decode mirrored QRs.
    int qw = 0, qh = 0;
    uint8_t *gray = quirc_begin(q, &qw, &qh);
    bool gray_ok = (gray && qw == (int)fb->width && qh == (int)fb->height);
    if (gray_ok) {
      rgb565_to_grayscale(fb->buf, gray, qw * qh);
    }

    // 2. Build the display preview: downsample 640x480 → 320x240 AND mirror
    //    horizontally in a single pass. dst(x,y) = src(2*(319-x), 2*y).
    //    Mirroring affects only the displayed preview, not what we feed
    //    to quirc.
    {
      const uint16_t *src = (const uint16_t *)fb->buf;
      const int SW = (int)fb->width;        // 640
      for (int y = 0; y < 240; ++y) {
        const uint16_t *src_row = src + (y * 2) * SW;
        uint16_t *dst_row = preview + y * 320;
        for (int x = 0; x < 320; ++x) {
          dst_row[x] = src_row[2 * (319 - x)];
        }
      }
    }

    // 3. Push the downsampled+mirrored frame to the display.
    M5.Display.pushImage(0, 0, 320, 240, preview);

    // 4. Run the decode pipeline on the un-mirrored VGA grayscale.
    if (gray_ok) {
      quirc_end(q);
    }

    // 3. Inspect any decoded codes.
    // NOTE: struct quirc_data is ~9 KB (mostly the payload buffer). The
    // Arduino loopTask only has an 8 KB stack — declaring these as locals
    // here would cause an immediate stack overflow + canary fault. Make
    // them static so they live in BSS instead of stack. wifi_setup_run is
    // called once per boot so reusing the same buffers is fine.
    static struct quirc_code code;
    static struct quirc_data data;

    int count = quirc_count(q);
    if (count > 0) {
      total_codes_seen++;
      Serial.printf("[setup] frame %lu: %d code(s) found\n",
                    (unsigned long)frame_count, count);
    }

    bool got_creds = false;
    for (int i = 0; i < count; ++i) {
      quirc_extract(q, i, &code);
      quirc_decode_error_t err = quirc_decode(&code, &data);
      if (err != QUIRC_SUCCESS) {
        if (err == QUIRC_ERROR_DATA_ECC || err == QUIRC_ERROR_FORMAT_ECC) {
          total_ecc_failures++;
        }
        Serial.printf("[setup]   code[%d] decode error: %s\n",
                      i, quirc_strerror(err));
        continue;
      }

      // Build a printable preview of the payload (replace non-ASCII with '.')
      char preview[64] = {0};
      int pl = data.payload_len < (int)sizeof(preview) - 1
               ? data.payload_len : (int)sizeof(preview) - 1;
      for (int j = 0; j < pl; ++j) {
        char c = (char)data.payload[j];
        preview[j] = (c >= 32 && c < 127) ? c : '.';
      }
      Serial.printf("[setup]   code[%d] decoded: %d bytes, type=%d, payload=\"%s\"%s\n",
                    i, data.payload_len, data.data_type, preview,
                    data.payload_len > pl ? "..." : "");

      if (parse_wifi_qr(data.payload, data.payload_len, out)) {
        Serial.printf("[setup]   parsed as WIFI: SSID='%s' (pass len=%u)\n",
                      out->ssid.c_str(),
                      (unsigned)out->password.length());
        got_creds = true;
        break;
      } else {
        Serial.println("[setup]   payload is not a WIFI: format, skipping");
      }
    }

    esp_camera_fb_return(fb);
    frame_count++;

    if (got_creds) {
      // Visual confirmation before exit.
      M5.Display.fillScreen(TFT_DARKGREEN);
      M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
      M5.Display.setTextSize(2);
      M5.Display.setCursor(10, 50);
      M5.Display.println("QR Detected!");
      M5.Display.setCursor(10, 90);
      M5.Display.printf("SSID: %.20s", out->ssid.c_str());
      M5.Display.setCursor(10, 130);
      M5.Display.println("Saving credentials...");
      delay(1500);

      bool saved = wifi_creds_save(out);
      Serial.printf("[setup] wifi_creds_save: %s\n", saved ? "OK" : "FAILED");
      free(preview);
      quirc_destroy(q);
      return saved;
    }

    // No on-screen overlay during scan — full camera preview unobstructed.
    // (Suppress unused-variable warnings if the compiler notices.)
    (void)last_status_ms;
    (void)total_codes_seen;
    (void)total_ecc_failures;

    // Heartbeat to serial every ~30 frames (~3-5 seconds) so we can confirm
    // the loop is alive even when no QR has been detected yet.
    if (frame_count % 30 == 0) {
      Serial.printf("[setup] heartbeat: frame %lu, codes_found=%d\n",
                    (unsigned long)frame_count, count);
    }
  }
}
