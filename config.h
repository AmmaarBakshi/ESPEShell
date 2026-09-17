#pragma once
// ============================================================================
//  ESPEShell - user configuration
//  Fill in your WiFi credentials and choose a login password before flashing.
// ============================================================================

// ---- WiFi ------------------------------------------------------------------
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// How long to wait for WiFi before giving up and running on Serial only (ms).
#define WIFI_TIMEOUT_MS 15000

// ---- Identity --------------------------------------------------------------
#define ESPE_HOSTNAME   "esp32"     // shown in the prompt and used for mDNS-ish name
#define ESPE_USER       "root"      // whoami / prompt user

// ---- Remote shell (Telnet) -------------------------------------------------
#define TELNET_PORT     23

// Login password for remote (Telnet) sessions.
// Leave as "" to disable the login prompt (NOT recommended on a shared WiFi).
// Can be changed at runtime with the `passwd` command (until reboot).
#define TELNET_PASSWORD "changeme"

// Max failed password attempts before the connection is dropped.
#define TELNET_MAX_TRIES 3

// ---- Version ---------------------------------------------------------------
#define ESPE_VERSION    "0.1.0"
