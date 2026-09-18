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

// ---- Board: ESP32 DevKit V1 (ESP32-WROOM-32 module, "ESP32 Dev Module") ----
// Flash/partition settings live in sketch.yaml (arduino-cli) / the Arduino IDE
// Tools menu (see README) - these two are just pin numbers used by commands.
#define ESPE_ONBOARD_LED_PIN  2   // most DevKit V1 boards: onboard blue LED
#define ESPE_BOOT_BUTTON_PIN  0   // the "BOOT" button (also a strapping pin)

// ---- Persistent storage (NVS, via Preferences) -----------------------------
// Namespace used for settings that should survive a reboot / reflash without
// being recompiled in, e.g. WiFi credentials set at runtime with `wifi set`.
#define ESPE_PREFS_NAMESPACE "espeshell"

// ---- Version ---------------------------------------------------------------
#define ESPE_VERSION    "0.2.0"
