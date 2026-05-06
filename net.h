// net.h — WiFi, mDNS, OTA, NTP plumbing.
//
// Spec ordering: WiFiManager.autoConnect runs first (its own captive portal
// HTTP server occupies port 80). Only after it exits do we bind the app's
// WebServer. mDNS and OTA must be re-issued on every reconnect, not just
// the first; that's wired through an SYSTEM_EVENT_STA_GOT_IP handler.

#pragma once

void net_begin();              // brings up WiFi (captive portal on first boot)
void net_loop();               // call from main loop: OTA poll + state mirror
void net_reset_credentials();  // wipes NVS creds and reboots

// Re-announce mDNS / refresh OTA hostname after device_hostname_set().
// mDNS updates take effect immediately; the WiFi DHCP-side hostname and
// OTA label fully refresh on the next reconnect/reboot.
void net_apply_hostname_change();

bool net_is_connected();
int  net_rssi();
