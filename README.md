# ESPEShell

A Unix-like shell for the **ESP32**, written as an Arduino sketch. You talk to it
over **USB Serial** or remotely over **Telnet** — the practical, ssh-like way to
reach an ESP32 (full SSH crypto does not fit comfortably on the chip).

It implements ~100 common shell commands (filesystem, text processing, search,
system/info, networking) on top of **LittleFS**, plus ESP-specific commands like
`tsw`, `pin`, `restart` and `data`. Pipes (`|`) and redirection (`>`, `>>`) work.

---

## 1. Requirements

- An ESP32 board (the pin notes assume a classic ESP32 dev board).
- Arduino IDE **2.x** *or* `arduino-cli`, with the **esp32** board package by
  Espressif installed.

The sketch uses only libraries bundled with the ESP32 core: `WiFi`, `HTTPClient`,
`WiFiClientSecure`, `LittleFS`, and FreeRTOS — nothing to install separately.

## 2. Configure (`config.h`)

Open `config.h` and set:

| Setting            | Meaning                                              |
| ------------------ | ---------------------------------------------------- |
| `WIFI_SSID`        | your WiFi network name                               |
| `WIFI_PASSWORD`    | your WiFi password                                   |
| `TELNET_PASSWORD`  | login password for remote sessions (`""` disables)   |
| `ESPE_HOSTNAME`    | hostname shown in the prompt                         |
| `TELNET_PORT`      | Telnet port (default 23)                             |

The telnet password can also be changed at runtime with `passwd <new>` (until
the next reboot).

## 3. Flash

This project is configured for an **ESP32 DevKit V1** (the classic 30/38-pin
board built around the ESP32-WROOM-32 module, no PSRAM).

**Arduino IDE:** open this folder (it contains `ESPEShell.ino`), then under
**Tools** set:

| Setting          | Value                          |
| ----------------- | ------------------------------- |
| Board              | **ESP32 Dev Module**            |
| Upload Speed       | 921600                          |
| Flash Frequency    | 80MHz                           |
| Flash Mode         | QIO                              |
| Flash Size         | 4MB (32Mb)                      |
| Partition Scheme   | Default 4MB with spiffs         |
| PSRAM              | Disabled                         |
| Core Debug Level   | None                             |

Pick your board's serial **Port**, then Upload.

**arduino-cli:** the same settings are pinned in `sketch.yaml`, so just:
```sh
arduino-cli compile .
arduino-cli upload -p <PORT> .
```

The "Default 4MB with spiffs" partition scheme includes NVS (for `wifi set`
and `dmesg`'s boot counter) and two OTA app slots (for the `ota` command), so
no partition changes are needed for anything in this project.

LittleFS is formatted automatically on first boot.

### DevKit V1 pin quick-reference

| Pins            | Notes                                                      |
| ---------------- | ------------------------------------------------------------ |
| GPIO 6-11         | Wired to the onboard SPI flash - never use these (`pin`/`pwm` refuse them) |
| GPIO 34-39        | Input-only (no `pinMode(OUTPUT)`, no PWM)                    |
| GPIO 0, 2, 5, 12, 15 | Strapping pins - affect boot mode; avoid driving them at boot |
| GPIO 0            | Also the **BOOT** button (`ESPE_BOOT_BUTTON_PIN` in `config.h`) |
| GPIO 2            | Onboard LED on most boards (`led on\|off\|toggle`)          |
| GPIO 21 / 22      | Default I2C SDA / SCL (used by `i2cscan` when no `-sda/-scl` given) |
| GPIO 0, 2, 4, 12-15, 25-27, 32-33 | Shared with ADC2 - readings can be unreliable while WiFi is active |

Some DevKit V1 clones wire the onboard LED to a different pin (or omit it) and
some drive it active-low - if `led on` doesn't light it, check your board's
schematic and adjust `ESPE_ONBOARD_LED_PIN` in `config.h`.

## 4. Connect

**Serial:** open the Serial Monitor at **115200 baud**. Set the line ending to
**Newline** (or CR) so pressing Enter submits a command. You get a prompt
immediately, before WiFi is up.

**Telnet (remote, ssh-like):** once WiFi connects, the boot log prints the IP.
From your PC:
```sh
telnet <device-ip>
```
Enter the password when prompted. PuTTY (Telnet mode) works too. One remote
session at a time; type `exit` (or close the terminal) to disconnect.

```
root@esp32:/$ ls -la
root@esp32:/$ echo hello > note.txt
root@esp32:/$ cat note.txt | wc -l
root@esp32:/$ pin --all
root@esp32:/$ data
root@esp32:/$ tsw
```

## 5. Commands

Type `help` for the full grouped list, `help <cmd>` for usage, or `help --esp`
for the ESP-specific commands. `man`, `whatis` and `apropos` also work.

Highlights:

- **Filesystem:** `ls cd pwd mkdir rmdir touch cp mv rm file stat tree du df
  basename dirname realpath`
- **Text:** `cat head tail wc sort uniq cut paste tr tee nl fold fmt od xxd
  strings diff cmp`
- **Search / scripting:** `grep rg find locate sed awk xargs which type command`
- **System:** `uname hostname uptime free whoami id who w passwd ps top pgrep`
- **Networking:** `ip ping curl wget dig nslookup ss`
- **ESP32:** `tsw pin pwm led restart data chip heap i2cscan wifiscan`

### ESP-specific

| Command                    | What it does                                            |
| --------------------------- | -------------------------------------------------------- |
| `tsw`                       | time since wake (uptime)                                 |
| `pin --status`               | list usable GPIOs with mode + level                      |
| `pin --all`                  | every GPIO 0–39 (flags flash / input-only / strapping)   |
| `pin --used` / `--free`      | pins you've configured / not yet configured              |
| `pin mode <n> in\|out\|up`   | set a pin's mode                                         |
| `pin read <n>` / `aread <n>` | digital / analog read                                    |
| `pin write <n> <0\|1>`       | drive a pin high/low                                     |
| `pwm <n> <duty> [freq]`      | PWM output on a pin (LEDC), duty 0–255, freq default 5kHz|
| `pwm <n> off` / `--status`   | stop PWM on a pin / list configured PWM pins             |
| `i2cscan [-sda P] [-scl P]`  | probe the I2C bus, print addresses that answer           |
| `wifiscan`                   | list nearby WiFi networks (RSSI, channel, security)      |
| `data`                       | live snapshot: heap, RSSI, uptime, bytes in/out, tasks   |
| `restart` / `reboot`         | reboot the ESP32                                         |
| `led on\|off\|toggle\|status`| onboard LED, DevKit V1 default GPIO2                     |
| `chip` / `heap`              | chip info / heap summary                                 |

## 6. Honest limitations

An ESP32 is not a Linux box, so some commands are **stubs** that print why they
can't run (they're still listed in `help` so nothing silently vanishes):

- `ssh`, `scp`, `sftp` — this device *is* the remote shell; connect **to** it.
- `tar`, `gzip`, `gunzip`, `zip`, `unzip` — no compression library is built in.
- `apt` — no package manager; commands are compiled into the firmware.
- `traceroute` — needs raw ICMP; use `ping` / `dig`.
- `kill`, `killall`, `pkill`, `bg`, `fg` — no user processes (see `ps` for
  FreeRTOS tasks).
- `chmod`, `chown`, `chgrp`, `ln` — LittleFS has no permissions/ownership/links.

And some are **useful subsets**, noted in `help`:

- `grep` is fixed-string (not full regex).
- `sed` supports `s/pat/rep/[g]` only.
- `awk` supports the `{print $N}` field-printer subset.
- `ping` is a TCP-connect reachability check (no raw ICMP).

## 7. Project layout

| File             | Contents                                                |
| ---------------- | ------------------------------------------------------- |
| `ESPEShell.ino`  | setup/loop, WiFi, Telnet server, serial, login, editor  |
| `shell.h/.cpp`   | dispatcher, pipes/redirection, paths, `help`, helpers   |
| `config.h`       | WiFi + login + identity settings                        |
| `fs_cmds.cpp`    | filesystem commands                                     |
| `text_cmds.cpp`  | text-processing commands                                |
| `search_cmds.cpp`| grep / find / sed / awk / xargs / which                 |
| `sys_cmds.cpp`   | system, info, process/job commands                      |
| `net_cmds.cpp`   | networking + env / echo / printf                        |
| `misc_cmds.cpp`  | archive / package-manager stubs                         |
| `esp_cmds.cpp`   | ESP32-specific commands                                 |

New commands are added by writing a handler and appending it to that module's
`Command[]` table — the `help` listing updates automatically.
