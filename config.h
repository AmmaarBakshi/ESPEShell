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

// Where `rgb` looks for a WS2812/NeoPixel by default. The plain DevKit V1 has
// no addressable LED, so this is only a starting point for a wired-up strip;
// boards that do have one (S3 DevKit, C3 Zero) usually put it on GPIO48 or 8.
#define ESPE_RGB_LED_PIN      48

// ---- Persistent storage (NVS, via Preferences) -----------------------------
// Namespace used for settings that should survive a reboot / reflash without
// being recompiled in, e.g. WiFi credentials set at runtime with `wifi set`.
#define ESPE_PREFS_NAMESPACE "espeshell"

// ---- MQTT (optional - requires the "PubSubClient" library by knolleary) ----
// Install via Arduino Library Manager: search "PubSubClient" (Nick O'Leary).
// This is the ONE dependency in this project not bundled with the ESP32 core.
// Leave MQTT_BROKER_HOST empty to require `mqtt connect <host> [port]` each
// time instead of a compiled-in default.
#define MQTT_BROKER_HOST  ""
#define MQTT_BROKER_PORT  1883

// ---- Host bridge (tools/espehost.py on the laptop) -------------------------
// The agent connects TO this port on the ESP32; the ESP32 never dials out.
// See host_cmds.cpp for the protocol and the reasoning.
#define HOST_BRIDGE_PORT       2323
#define HOST_BRIDGE_TIMEOUT_MS 5000    // per-request wait before giving up
#define HOST_BRIDGE_LINE_MAX   8192    // longest reply line we will buffer

// ---- Fusion mode (fuse_cmds.cpp) -------------------------------------------
// How often rules are evaluated, and the per-request budget when a rule's
// source lives on the laptop. The host timeout is shorter than
// HOST_BRIDGE_TIMEOUT_MS on purpose: rule evaluation runs on the main loop,
// so a slow agent must not stall the shell for the full interactive timeout.
#define FUSE_INTERVAL_MS     5000
#define FUSE_HOST_TIMEOUT_MS 2000
#define FUSE_RULES_PATH      "/fuse.rules"

// ---- Version ---------------------------------------------------------------
#define ESPE_VERSION    "0.3.0"
