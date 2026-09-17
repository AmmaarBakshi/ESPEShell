# ESPEShell

A Unix-like shell for the **ESP32**, written as an Arduino sketch. Talk to it over
**USB Serial** or remotely over **Telnet** (the practical, ssh-like way to reach an
ESP32 — full SSH crypto does not fit comfortably on the chip).

It implements the common shell commands (filesystem, text processing, search,
system/info, networking) on top of **LittleFS**, plus ESP-specific commands like
`tsw`, `pin`, `restart` and `data`.

> Status: work in progress. See `CLAUDE.md` for the command wishlist.

## Quick start
1. Open this folder in the Arduino IDE (or use `arduino-cli`).
2. Install the **esp32** board package (Espressif) if you have not already.
3. Edit `config.h`: set `WIFI_SSID`, `WIFI_PASSWORD`, and a `TELNET_PASSWORD`.
4. Select your ESP32 board and flash.
5. Open the Serial Monitor at **115200 baud** — you get a prompt immediately.
6. Once WiFi connects, note the printed IP and run `telnet <ip>` from your PC.

Detailed flashing, wiring and command notes are added to this file as the project
is built out.
