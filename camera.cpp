// camera.cpp — GC0308 camera driver, /snapshot backend.
//
// Init pattern is cribbed from M5Stack's official M5CoreS3 library
// (m5stack/M5CoreS3, src/utility/GC0308.cpp). Key insights versus naive
// esp32-camera examples:
//
//   - The CoreS3's GC0308 uses its own internal clock — pin_xclk is -1, NOT
//     GPIO 2. Earlier M5 doc examples for OTHER boards listed XCLK on GPIO 2;
//     copying that pin map silently misconfigures the camera and breaks
//     Port A (which actually IS GPIO 2's normal duty). The CoreS3 GC0308
//     pin map differs significantly from common esp32-camera examples.
//   - `M5.In_I2C.release()` MUST be called before `esp_camera_init()`. This
//     releases the internal I2C bus from M5Unified so the camera driver can
//     take it over for SCCB. Without this, init fails with a GDMA error and
//     leaves GPIO 11/12 in a bad state, killing Wire1.
//   - The GC0308 captures RGB565 (no hardware JPEG). For /snapshot we
//     convert each frame to JPEG via frame2jpg() at request time.
//
// Capture is on demand from the synchronous WebServer handler, serialized
// via an internal mutex. The handler holds the lock from camera_get_frame()
// through camera_release_frame(); during that window the JPEG buffer is
// stable.

#include <Arduino.h>
#include <M5Unified.h>
#include <Wire.h>
#include <esp_camera.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "camera.h"
#include "config.h"

// ---------------------------------------------------------------------------
// CoreS3 GC0308 pin map. Authoritative source: m5stack/M5CoreS3
// src/utility/GC0308.cpp on the main branch.
// ---------------------------------------------------------------------------
static camera_config_t s_config = {
  .pin_pwdn       = -1,
  .pin_reset      = -1,
  .pin_xclk       = -1,    // GC0308 has its own clock; do NOT drive XCLK
  .pin_sccb_sda   = 12,
  .pin_sccb_scl   = 11,
  .pin_d7         = 47,
  .pin_d6         = 48,
  .pin_d5         = 16,
  .pin_d4         = 15,
  .pin_d3         = 42,
  .pin_d2         = 41,
  .pin_d1         = 40,
  .pin_d0         = 39,
  .pin_vsync      = 46,
  .pin_href       = 38,
  .pin_pclk       = 45,
  .xclk_freq_hz   = 20000000,
  .ledc_timer     = LEDC_TIMER_0,
  .ledc_channel   = LEDC_CHANNEL_0,
  .pixel_format   = PIXFORMAT_RGB565,    // GC0308 has no hardware JPEG
  .frame_size     = FRAMESIZE_VGA,       // 640x480 — native sensor res; gives
                                          // ~6-7 px/module on a phone-screen
                                          // QR at the GC0308's hyperfocal
                                          // distance, vs. ~3 px/module at QVGA.
  .jpeg_quality   = 0,                    // unused with RGB565 capture
  .fb_count       = 2,                    // double-buffer — camera fills the
                                          // next frame while we process the
                                          // previous, reducing the effective
                                          // motion-blur window during QR scan.
  .fb_location    = CAMERA_FB_IN_PSRAM,
  .grab_mode      = CAMERA_GRAB_LATEST,    // always return the most recent
                                            // frame; older queued frames are
                                            // dropped. Better for live scan
                                            // (no stale-frame lag) and for
                                            // /snapshot (most current view).
  .sccb_i2c_port  = -1,                   // let driver allocate
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_lock        = nullptr;
static camera_fb_t      *s_current_fb  = nullptr;
static uint8_t          *s_jpeg_buf    = nullptr;   // alloc by frame2jpg
static size_t            s_jpeg_len    = 0;
static bool              s_init_ok     = false;
static int               s_last_err    = 0;         // last esp_camera_init error

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void camera_start() {
  Serial.println("[camera] start");
  s_lock = xSemaphoreCreateMutex();

  // Hand the internal I2C bus over to the camera driver for SCCB use.
  Serial.println("[camera] M5.In_I2C.release()");
  M5.In_I2C.release();

  Serial.printf("[camera] free PSRAM before init: %u bytes\n",
                (unsigned)ESP.getFreePsram());
  Serial.println("[camera] calling esp_camera_init...");
  esp_err_t err = esp_camera_init(&s_config);
  s_last_err = (int)err;

  // DO NOT touch Wire1 here. esp_camera_init takes I2C bus 1 for SCCB, but
  // M5.In_I2C.readRegister/writeRegister still work for OTHER addresses on
  // the same bus — provided we don't call Wire1.end()/Wire1.begin() (that
  // breaks the bus state) and provided the I2C frequency is throttled to
  // <=100 kHz. Reference: m5stack/CoreS3-UserDemo, AppCameraModel.cpp,
  // which initializes the camera AND reads the LTR-553 from the same class.

  if (err != ESP_OK) {
    Serial.printf("[camera] esp_camera_init FAILED: 0x%04x\n", (unsigned)err);
    s_init_ok = false;
    return;
  }
  Serial.println("[camera] esp_camera_init OK");

  // Sensor tuning for QR scanning of phone screens.
  //
  // The GC0308's auto-exposure tends to meter for the entire scene, which
  // means a bright phone screen against a dark room background gets
  // massively overexposed — the QR's white area clips to pure white and
  // the data modules become invisible. We bias the auto-exposure toward
  // a darker target (ae_level = -2) so phone-screen highlights stay
  // below saturation. We also drop contrast back to neutral; pushing
  // contrast on a clipped image just discards information.
  if (sensor_t *sensor = esp_camera_sensor_get()) {
    sensor->set_contrast(sensor, 1);                  // mild contrast bump
    sensor->set_brightness(sensor, 0);                // neutral
    sensor->set_saturation(sensor, 0);                // doesn't affect grayscale
    sensor->set_special_effect(sensor, 0);            // 0 = no effect
    sensor->set_whitebal(sensor, 1);                  // auto WB on
    sensor->set_exposure_ctrl(sensor, 1);             // auto exposure on
    // ae_level = -1 keeps phone-screen highlights from clipping while
    // still letting the QR's white background sit in the bright range
    // (~180/255), so the local contrast against dark modules (~130/255)
    // is large enough for quirc's adaptive threshold to work cleanly.
    // -2 was too aggressive and pushed everything into mid-grey.
    sensor->set_ae_level(sensor, -1);
    sensor->set_gain_ctrl(sensor, 1);                 // auto gain on, but...
    sensor->set_gainceiling(sensor, GAINCEILING_2X);  // ...cap at 2x to limit noise
  }

  // Discard the first frame (GC0308 first-integration window is unreliable).
  Serial.println("[camera] capturing first-frame discard...");
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) {
    Serial.printf("[camera] first frame discarded (%u bytes)\n", (unsigned)fb->len);
    esp_camera_fb_return(fb);
  } else {
    Serial.println("[camera] WARNING: no first frame available to discard");
  }

  s_init_ok = true;
  Serial.println("[camera] init complete");
}

bool camera_get_frame(CameraFrame *out, uint32_t timeout_ms) {
  if (!out || !s_init_ok || !s_lock) return false;

  if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return false;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(s_lock);
    return false;
  }

  // Convert RGB565 → JPEG. quality is 0–100; 80 is a reasonable balance.
  // frame2jpg() allocates s_jpeg_buf via malloc; camera_release_frame()
  // is responsible for freeing it.
  if (!frame2jpg(fb, 80, &s_jpeg_buf, &s_jpeg_len)) {
    Serial.println("[camera] frame2jpg failed");
    esp_camera_fb_return(fb);
    xSemaphoreGive(s_lock);
    return false;
  }

  s_current_fb = fb;
  out->jpeg    = s_jpeg_buf;
  out->len     = s_jpeg_len;
  out->ok      = true;
  return true;   // caller MUST pair with camera_release_frame()
}

void camera_release_frame() {
  if (s_jpeg_buf) {
    free(s_jpeg_buf);
    s_jpeg_buf = nullptr;
    s_jpeg_len = 0;
  }
  if (s_current_fb) {
    esp_camera_fb_return(s_current_fb);
    s_current_fb = nullptr;
  }
  if (s_lock) {
    xSemaphoreGive(s_lock);
  }
}

void camera_stop() {
  if (!s_init_ok) {
    Serial.println("[camera] stop: not running, nothing to do");
    return;
  }

  // Take the lock so no /snapshot handler is mid-conversion when we yank
  // the driver out from under it. If a snapshot is in-flight we just wait;
  // the synchronous WebServer means at most one outstanding capture.
  if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
    Serial.println("[camera] stop: timeout waiting for lock; deinit-ing anyway");
  }

  // Free anything dangling from a recent get_frame the handler didn't
  // get to release (defensive; under normal flow these are already null).
  if (s_jpeg_buf) {
    free(s_jpeg_buf);
    s_jpeg_buf = nullptr;
    s_jpeg_len = 0;
  }
  if (s_current_fb) {
    esp_camera_fb_return(s_current_fb);
    s_current_fb = nullptr;
  }

  Serial.printf("[camera] stop: free PSRAM before deinit: %u bytes\n",
                (unsigned)ESP.getFreePsram());
  esp_err_t err = esp_camera_deinit();
  if (err != ESP_OK) {
    Serial.printf("[camera] esp_camera_deinit returned 0x%04x\n", (unsigned)err);
  }
  Serial.printf("[camera] stop: free PSRAM after deinit:  %u bytes\n",
                (unsigned)ESP.getFreePsram());

  s_init_ok = false;

  if (s_lock) {
    xSemaphoreGive(s_lock);
  }
}

bool camera_is_initialized() { return s_init_ok; }
int  camera_last_error()     { return s_last_err; }
