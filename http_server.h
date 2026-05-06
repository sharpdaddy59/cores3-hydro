// http_server.h — synchronous WebServer wrapper.
//
// `WebServer.h` is single-threaded: handle_client() must be polled from the
// main loop. That is intentional — the device serves one client and the
// camera path needs to block, both of which fight AsyncWebServer.

#pragma once

void http_server_begin();
void http_server_loop();
