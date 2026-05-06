// ota.h — HTTP-based OTA firmware update.
//
// Adds two endpoints (registered into the project's existing WebServer):
//
//   GET  /ota          — minimal HTML page with a file picker and progress UI
//   POST /ota/upload   — multipart upload of a raw .bin firmware image
//
// This is a parallel path to the ArduinoOTA push from build host (see
// net.cpp). ArduinoOTA stays for the dev workflow (`build.ps1 -Network`);
// /ota/upload is for browser-driven updates from a phone or laptop.
//
// Architecture: the upload is buffered in PSRAM in its entirety before any
// byte hits flash (ps_malloc → receive → validate → Update.begin/write/end).
// This is safer than streaming straight to flash — a torn upload leaves the
// running partition untouched. Camera framebuffers are released first to
// make room for the buffer.

#pragma once

class WebServer;

// Register /ota and /ota/upload on the given WebServer. Must be called
// from http_server_begin() (the project's only WebServer is owned there).
void ota_register(WebServer &server);
