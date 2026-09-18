# ESPEShell

A Unix-like shell for the **ESP32**, written as an Arduino sketch. You talk to it
over **USB Serial** or remotely over **Telnet** — the practical, ssh-like way to
reach an ESP32 (full SSH crypto does not fit comfortably on the chip).

It implements ~100 common shell commands (filesystem, text processing, search,
system/info, networking) on top of **LittleFS**, plus ESP-specific commands like
`tsw`, `pin`, `pwm`, `restart` and `data`. Pipes (`|`) and redirection (`>`, `>>`)
work, and so does command history (Up/Down) and Tab completion. WiFi credentials
can be set at runtime (no reflash needed) and firmware can be updated over the
air.

---

## 1. Requirements

- An ESP32 board (this project is configured for an **ESP32 DevKit V1** — see
  section 3).
- Arduino IDE **2.x** *or* `arduino-cli`, with the **esp32** board package by
  Espressif installed.

The sketch uses only libraries bundled with the ESP32 core - `WiFi`, `HTTPClient`,
`WiFiClientSecure`, `LittleFS`, `Preferences`, `Update`, `ESPmDNS`, `Wire`, and
FreeRTOS - nothing to install separately.

## 2. Configure (`config.h`)

Open `config.h` and set:

| Setting            | Meaning                                              |
| ------------------ | ---------------------------------------------------- |
| `WIFI_SSID`        | initial WiFi network name (fallback - see `wifi` below) |
| `WIFI_PASSWORD`    | initial WiFi password (fallback)                     |
| `TELNET_PASSWORD`  | login password for remote sessions (`""` disables)   |
| `ESPE_HOSTNAME`    | hostname shown in the prompt and used for `<host>.local` |
| `TELNET_PORT`      | Telnet port (default 23)                             |
| `ESPE_ONBOARD_LED_PIN` | onboard LED pin used by `led` (DevKit V1 default: GPIO2) |

`WIFI_SSID`/`WIFI_PASSWORD` are only used the **first** boot, or whenever no
credentials have been saved with `wifi set` - see section 5. The telnet
password can be changed at runtime with `passwd <new>` (until reboot).

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
| GPIO 0            | Also the **BOOT** button (`ESPE_BOOT_BUTTON_PIN` in `config.h`); also RTC-capable (deep sleep wake) |
| GPIO 2            | Onboard LED on most boards (`led on\|off\|toggle`)          |
| GPIO 21 / 22      | Default I2C SDA / SCL (used by `i2cscan` when no `-sda/-scl` given) |
| GPIO 0, 2, 4, 12-15, 25-27, 32-39 | RTC GPIOs - the only pins `deepsleep <secs> pinN <0\|1>` can wake on |
| GPIO 0, 2, 4, 12-15, 25-27, 32-33 | Shared with ADC2 - readings can be unreliable while WiFi is active |

Some DevKit V1 clones wire the onboard LED to a different pin (or omit it) and
some drive it active-low - if `led on` doesn't light it, check your board's
schematic and adjust `ESPE_ONBOARD_LED_PIN` in `config.h`.

## 4. Connect

**Serial:** open the Serial Monitor at **115200 baud**. Set the line ending to
**Newline** (or CR) so pressing Enter submits a command. You get a prompt
immediately, before WiFi is up.

**Telnet (remote, ssh-like):** once WiFi connects, the boot log prints the IP
(and `<hostname>.local` if mDNS started - see section 5). From your PC:
```sh
telnet <device-ip>
# or, once WiFi is up:
telnet esp32.local
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

**Command history & Tab completion:** press **Up/Down** to recall previous
commands, and **Tab** to complete a command name or a path (ambiguous matches
are listed; a unique match is filled in). This relies on ANSI escape codes
(`\r`, cursor/line erase), which real terminals and Telnet clients (PuTTY, a
Linux/macOS `telnet`, etc.) support - the plain **Arduino IDE Serial Monitor
does not** interpret them, so history/Tab will look garbled there even though
typing, backspace, and Enter still work fine. Use a real serial terminal
(PuTTY, `tio`, `minicom`, ...) over USB to get the full experience on Serial.

## 5. WiFi configuration & mDNS

WiFi credentials don't have to be baked into `config.h` - set them at runtime
and they persist across reboots in NVS flash:

```
root@esp32:/$ wifi status
root@esp32:/$ wifi set MyNetwork "my password"
root@esp32:/$ wifi forget          # revert to config.h defaults on next boot
```

`wifi set` connects immediately (so you can confirm it worked) and saves the
credentials either way, so a firmware reflash is never needed to change
networks. NVS storage isn't encrypted by default (same caveat as `config.h`
being plaintext) - don't reuse a sensitive password here on a device others
can access.

Once connected, the device also advertises itself over **mDNS** as
`<hostname>.local` (`esp32.local` by default), so `telnet esp32.local` works
without knowing the IP. Changing the hostname with `hostname <name>` updates
the prompt and WiFi client name immediately, but mDNS keeps the boot-time name
until the next reboot.

## 6. OTA firmware updates

Since ESPEShell is already a remote shell, it can update its own firmware over
WiFi instead of requiring a USB cable every time:

```
root@esp32:/$ ota http://192.168.1.50:8000/ESPEShell.ino.bin
```

Build the `.bin` with **Sketch > Export Compiled Binary** (Arduino IDE) or
`arduino-cli compile .` (it's written under `build/`), host it anywhere
reachable from the device (a `python3 -m http.server` on your PC works fine),
and point `ota` at the URL. It downloads, flashes into the inactive OTA slot,
and reboots into the new firmware. HTTPS URLs work too (certificate
validation is skipped, matching `curl`/`wget`). If it's interrupted or the
download is corrupt, `Update` refuses to boot the bad image and the previous
firmware stays in place.

## 7. File transfer (send / recv)

`ssh`/`scp`/`sftp` aren't feasible on a stock ESP32 (see Honest limitations
below), so real file transfer here is **base64-over-the-existing-connection**
- plain text that works over Telnet or Serial with no extra client software:

```
root@esp32:/$ send /note.txt
----BEGIN /note.txt (base64)----
aGVsbG8gd29ybGQK
----END----
.
(12 bytes)
root@esp32:/$ recv /copy.txt
Paste base64 data now (BEGIN/END framing is ignored). End with a line containing only: .
aGVsbG8gd29ybGQK
.
recv: wrote 12 bytes to /copy.txt
```

Copy `send`'s output (or just the base64 lines) and paste it into a `recv`
session - the terminating `.` line lets `recv` stop automatically. This is
simpler than XMODEM (no client tooling, works by copy-paste) at the cost of
~33% size overhead versus a true binary protocol; fine for config files,
scripts, and small assets.

## 8. Scripting (`sh`, `/boot.sh`)

`sh <file>` runs a LittleFS file as a sequence of shell commands, one per
line (blank lines and `#` comments are skipped):

```
root@esp32:/$ cat /setup.sh
# turn on the LED and show status
led on
data
root@esp32:/$ sh /setup.sh
```

If a file named **`/boot.sh`** exists, it runs automatically at startup
(after WiFi/mDNS/Telnet are up), printed to Serial - handy for auto-starting
a `pin`/`pwm` configuration, printing a custom banner line, etc.

## 9. Commands

Type `help` for the full grouped list, `help <cmd>` for usage, or `help --esp`
for the ESP-specific commands. `man`, `whatis` and `apropos` also work.

Highlights:

- **Filesystem:** `ls cd pwd mkdir rmdir touch cp mv rm file stat tree du df
  basename dirname realpath`
- **Text:** `cat head tail wc sort uniq cut paste tr tee nl fold fmt od xxd
  strings diff cmp`
- **Search / scripting:** `grep rg find locate sed awk xargs which type command`
- **Shell built-ins:** `help man clear sh exit`
- **System:** `uname hostname uptime free whoami id who w passwd ps top pgrep`
- **Networking:** `ip wifi ping curl wget dig nslookup ss`
- **File transfer:** `send recv`
- **ESP32:** `tsw pin pwm led sleep deepsleep restart ota dmesg data chip heap
  i2cscan wifiscan`

### ESP-specific

| Command                       | What it does                                            |
| ------------------------------ | -------------------------------------------------------- |
| `tsw`                          | time since wake (uptime)                                 |
| `pin --status` / `--all` / `--used` / `--free` | inspect GPIOs (mode, level, flags)        |
| `pin mode <n> in\|out\|up`     | set a pin's mode                                         |
| `pin read <n>` / `aread <n>`   | digital / analog read                                    |
| `pin write <n> <0\|1>`         | drive a pin high/low                                     |
| `pwm <n> <duty> [freq]` / `off` / `--status` | PWM output on a pin (LEDC), duty 0-255      |
| `led on\|off\|toggle\|status`  | onboard LED, DevKit V1 default GPIO2                     |
| `i2cscan [-sda P] [-scl P]`    | probe the I2C bus, print addresses that answer           |
| `wifiscan`                     | list nearby WiFi networks (RSSI, channel, security)      |
| `sleep <secs>`                 | light sleep (RAM kept; WiFi/Telnet likely drops)         |
| `deepsleep <secs> [pinN 0\|1]` | deep sleep - resets on wake (timer and/or GPIO wake)     |
| `ota <url>`                    | download + flash firmware over HTTP(S)                   |
| `dmesg`                        | reset reason, sleep-wake cause, persisted boot count     |
| `data`                         | live snapshot: heap, RSSI, uptime, bytes in/out, tasks   |
| `restart` / `reboot`           | reboot the ESP32                                         |
| `chip` / `heap`                | chip info / heap summary                                 |

## 10. Honest limitations

An ESP32 is not a Linux box, so some commands are **stubs** that print why they
can't run (they're still listed in `help` so nothing silently vanishes):

- `ssh`, `scp`, `sftp` — this device *is* the remote shell; connect **to** it,
  or use `send`/`recv` (section 7) for actual file transfer.
- `tar`, `gzip`, `gunzip`, `zip`, `unzip` — no compression library is built in.
- `apt` — no package manager; commands are compiled into the firmware.
- `traceroute` — needs raw ICMP; use `ping` / `dig`.
- `kill`, `killall`, `pkill`, `bg`, `fg` — no user processes (see `ps` for
  FreeRTOS tasks).
- `chmod`, `chown`, `chgrp`, `ln` — LittleFS has no permissions/ownership/links.

And some are **useful subsets or scoped-down**, noted in `help`/their own output:

- `grep` is fixed-string (not full regex).
- `sed` supports `s/pat/rep/[g]` only.
- `awk` supports the `{print $N}` field-printer subset.
- `ping` is a TCP-connect reachability check (no raw ICMP).
- `send`/`recv` are base64-over-the-connection, not XMODEM (see section 7).
- `dmesg` reports the reset reason, sleep-wake cause, and boot count - it does
  not decode full panic backtraces or core dumps.
- `sleep` (light sleep) and especially a Telnet session across it: the
  connection may drop and need reconnecting after waking - the command still
  blocks and sleeps exactly as asked, this is an honest side effect of the
  radio pausing.

## 11. Project layout

| File              | Contents                                                |
| ----------------- | ------------------------------------------------------- |
| `ESPEShell.ino`   | setup/loop, WiFi, Telnet server, serial, login, line editor (history/Tab) |
| `shell.h/.cpp`    | dispatcher, pipes/redirection, paths, history, tab completion, `help`, helpers |
| `config.h`        | WiFi + login + identity + board pin settings             |
| `sketch.yaml`     | pins the ESP32 DevKit V1 board profile for `arduino-cli` |
| `fs_cmds.cpp`     | filesystem commands                                      |
| `text_cmds.cpp`   | text-processing commands                                 |
| `search_cmds.cpp` | grep / find / sed / awk / xargs / which                  |
| `sys_cmds.cpp`    | system, info, process/job commands                       |
| `net_cmds.cpp`    | networking + `wifi` (NVS config) + env / echo / printf   |
| `xfer_cmds.cpp`   | `send` / `recv` base64 file transfer                     |
| `misc_cmds.cpp`   | archive / package-manager stubs                          |
| `esp_cmds.cpp`    | ESP32-specific commands (GPIO, PWM, sleep, OTA, dmesg, ...) |

New commands are added by writing a handler and appending it to that module's
`Command[]` table — the `help` listing updates automatically.
