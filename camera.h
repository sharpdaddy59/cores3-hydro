// camera.h — dedicated camera task + JPEG queue API.
//
// The /snapshot HTTP handler calls camera_get_frame() with a millisecond
// timeout. Internally, that maps to xQueuePeek with a tick deadline against
// the camera task's output queue — which is what gives the HTTP request a
// real bounded latency, even if esp_camera_fb_get() blocks indefinitely.
//
// The handler MUST call camera_release_frame() after using the buffer so the
// camera task can return the underlying esp_camera_fb_t to the driver.

#pragma once

#include <stddef.h>
#include <stdint.h>

struct CameraFrame {
  const uint8_t *jpeg;
  size_t         len;
  bool           ok;     // false on capture failure (handler should 503)
};

void camera_start();
bool camera_get_frame(CameraFrame *out, uint32_t timeout_ms);
void camera_release_frame();

// Tear down the camera and release its PSRAM framebuffers (one VGA RGB565
// frame is ~600 KB; with fb_count=2 that's ~1.2 MB). The OTA upload path
// calls this so the firmware buffer can ps_malloc() the largest contiguous
// region available. After this call camera_get_frame() will fail until
// camera_start() is invoked again — but the typical OTA flow rebooots
// before that matters. Safe to call when the camera was never initialized
// (no-op).
void camera_stop();

// Diagnostic getters — exposed via /status when serial logging is unreliable.
bool camera_is_initialized();
int  camera_last_error();        // 0 = no error or not yet attempted
