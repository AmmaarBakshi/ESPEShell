# ESPEShell

A Unix-like shell for the **ESP32**, written as an Arduino sketch. You talk to it
over **USB Serial** or remotely over **Telnet** — the practical, ssh-like way to
reach an ESP32 (full SSH crypto does not fit comfortably on the chip).

It implements **230+ commands** (filesystem, text processing, search,
system/info, networking) on top of **LittleFS**, plus ESP-specific commands like
`tsw`, `pin`, `pwm`, `restart` and `data`. Pipes (`|`) and redirection (`<`, `>`, `>>`)
work, and so does command history (Up/Down) and Tab completion. WiFi credentials
can be set at runtime (no reflash needed) and firmware can be updated over the
air.

Two things here are not in an ordinary shell:

- **The host bridge** (section 11). A small agent on your laptop lets the ESP32
  shell inspect that machine — battery and live current draw, cameras (including
  an ASCII preview of a frame), the screen, USB/HID/audio devices, CPU, memory,
  disks, network, processes.
- **Fusion mode** (section 12). Rules that wire the two together, in either
  direction: laptop CPU load driving a GPIO pin, a sensor on the board popping a
  notification on the laptop.

---

## 1. Requirements

- An ESP32 board (this project is configured for an **ESP32 DevKit V1** — see
  section 3).
- Arduino IDE **2.x** *or* `arduino-cli`, with the **esp32** board package by
  Espressif installed.
- **One external library:** [`PubSubClient`](https://github.com/knolleary/pubsubclient)
  by knolleary, for the `mqtt` command - install it via **Sketch > Include
  Library > Manage Libraries...** (search "PubSubClient") in the Arduino IDE,
  or `arduino-cli lib install PubSubClient`. The sketch will not compile
  without it.

Everything else uses libraries bundled with the ESP32 core - `WiFi`,
`HTTPClient`, `WiFiClientSecure`, `LittleFS`, `Preferences`, `Update`,
`ESPmDNS`, `Wire`, and FreeRTOS - nothing else to install.

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
| Partition Scheme   | **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** |
| PSRAM              | Disabled                         |
| Core Debug Level   | None                             |

Pick your board's serial **Port**, then Upload.

**arduino-cli:** the same settings are pinned in `sketch.yaml`, so just:
```sh
arduino-cli compile .
arduino-cli upload -p <PORT> .
```

The partition scheme matters: the firmware no longer fits the 1.31MB app slot
of "Default 4MB with spiffs", so pick **Minimal SPIFFS** - 1.9MB per app slot,
which still gives NVS (for `wifi set`, the timezone and `dmesg`'s boot counter)
and two OTA slots (for the `ota` command). The trade is a smaller filesystem:
about 190KB of LittleFS for your files. If you don't need `ota`, "No OTA (2MB
APP/2MB SPIFFS)" swaps that back for a much larger filesystem.

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

## 9. MQTT (optional - needs the PubSubClient library)

For home-automation/IoT use, `mqtt` connects to a broker and can publish and
subscribe:

```
root@esp32:/$ mqtt connect 192.168.1.10
root@esp32:/$ mqtt sub home/esp32/cmd
root@esp32:/$ mqtt pub home/esp32/status "up 3h"
root@esp32:/$
[mqtt] home/esp32/cmd: restart
root@esp32:/$
```

Incoming messages on a subscribed topic print as `[mqtt] topic: payload`
between prompts (in both Serial and a live Telnet session) without disturbing
whatever you're mid-typing. `mqtt status` shows the connection state and
subscribed topics; `mqtt pub <topic> <msg> [-r]` add `-r` to retain. Leave
`MQTT_BROKER_HOST` in `config.h` set so `mqtt connect` needs no arguments, or
pass a host (and optional port) each time. Plain MQTT only (port 1883,
`WiFiClient`) - MQTT-over-TLS (8883) isn't wired up. The default packet
buffer is ~256 bytes (topic + payload combined), plenty for typical
on/off/sensor-reading messages but not large payloads.

## 10. Commands

Type `help` for the full grouped list, `help <cmd>` for usage, `help --esp`
for the ESP-specific commands, or `help --host` for the laptop bridge.
`man`, `whatis` and `apropos` also work.

Highlights:

- **Filesystem:** `ls cd pwd pushd popd dirs mkdir rmdir touch cp mv rm file
  stat tree du df basename dirname realpath readlink truncate dd sync`
- **Text:** `cat head tail tac rev shuf wc sort uniq cut paste tr tee nl fold
  fmt od xxd strings diff cmp comm join expand unexpand column sponge bytes`
- **Data:** `md5sum sha256sum crc32 cksum base64 rand uuid jq bc factor`
- **Search / scripting:** `grep rg find locate sed awk xargs which type command
  watch repeat time seq yes true false test [ expr`
- **Shell built-ins:** `help man clear sh exit alias unalias history`
- **System:** `uname arch nproc hostname uptime free whoami id who w logname tty
  passwd ps top pgrep printenv date ntp cal every`
- **Networking:** `ip mac wifi ap httpd mqtt ping curl wget http nc dig nslookup
  ss portscan netscan mdns`
- **File transfer:** `send recv` (and `httpd` for a browser)
- **ESP32:** `tsw pin pinout pinwatch pwm adc dac tone beep servo touchpin led
  blink rgb sleep deepsleep restart ota dmesg data chip heap temp cpufreq bench
  nvs neofetch i2cscan i2c spi wifiscan efuse parts psram rtcmem wdt`
- **Sensors / signals:** `dht sonar pulse freq scope logic shiftout`
- **Laptop bridge:** `host hsys hcpu hmem hdisk hpower hcam hscreen hio hnet
  hproc hgpu htemp hnotify hexec htype hkey hclip` (section 11)
- **Fusion mode:** `fuse` (section 12)

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
| `blink [-secs] [-per-sec]`     | flash the onboard LED; default 10s at 1/sec, Ctrl-C stops |
| `i2cscan [-sda P] [-scl P]`    | probe the I2C bus, print addresses that answer           |
| `wifiscan`                     | list nearby WiFi networks (RSSI, channel, security)      |
| `sleep <secs>`                 | light sleep (RAM kept; WiFi/Telnet likely drops)         |
| `deepsleep <secs> [pinN 0\|1]` | deep sleep - resets on wake (timer and/or GPIO wake)     |
| `ota <url>`                    | download + flash firmware over HTTP(S)                   |
| `dmesg`                        | reset reason, sleep-wake cause, persisted boot count     |
| `data`                         | live snapshot: heap, RSSI, uptime, bytes in/out, tasks   |
| `restart` / `reboot`           | reboot the ESP32                                         |
| `chip` / `heap`                | chip info / heap summary                                 |
| `pinout`                       | DevKit V1 pin map with strapping / ADC warnings          |
| `pinwatch <n> [-up] [-t s]`    | log a pin's edges live until Ctrl-C                      |
| `adc <n> [-n N] [-d ms]`       | analog read: raw + millivolts, averaged over N samples   |
| `dac <25\|26> <0-255\|1.8v\|off>` | true analog output on the two DAC pins              |
| `tone <n> <hz> [ms]` / `beep`  | square wave for a piezo; `tone <n> off` stops it         |
| `servo <n> <0-180\|1500us\|off>` | hobby servo straight off LEDC, no library             |
| `touchpin <n> [-n N]`          | capacitive touch reading                                 |
| `i2c get\|set\|dump <addr> ...` | read / write a device's registers after `i2cscan`      |
| `temp`                         | internal die temperature (trend, not a thermometer)      |
| `cpufreq [mhz]`                | show / set CPU clock (240..10 MHz; <80 stops the radio)  |
| `bench [cpu\|fs]`              | quick CPU and LittleFS throughput benchmark              |
| `nvs [list\|get\|set\|rm\|clear]` | browse the settings the shell persists in NVS       |
| `neofetch` / `sysinfo`         | the whole board on one screen                            |
| `mkfs --force`                 | reformat LittleFS (erases everything)                    |

### Shell, time and background work

| Command                        | What it does                                             |
| ------------------------------ | -------------------------------------------------------- |
| `alias ll='ls -l'` / `unalias` | command aliases (RAM; put them in `/boot.sh` to persist) |
| `history [-c]`                 | list or clear the command history                        |
| `watch [-n secs] [-t] <cmd>`   | re-run a command on screen until Ctrl-C                  |
| `repeat N [-d ms] <cmd>`       | run a command N times                                    |
| `every <secs> <cmd>`           | run it in the background; `--list`, `--del N`, `--clear` |
| `time <cmd>`                   | how long a command took, plus its heap delta             |
| `seq` / `yes` / `true` / `false` | the small scripting primitives (all bounded)           |
| `test EXPR` / `[ EXPR ]` / `expr` | conditions and integer arithmetic for `sh` scripts    |
| `date [-u] [+FMT]` / `date -s` | wall clock, once `ntp` (or `-s`) has set it              |
| `ntp [sync [srv]\|tz H\|status]` | SNTP sync; the timezone offset is saved in NVS        |
| `cal [[month] year]`           | month calendar                                           |
| `md5sum` / `sha256sum` / `crc32` | digests of files or piped text                         |
| `base64 [-d]`                  | encode / decode                                          |
| `rand [-n N] [lo] hi` / `uuid` | hardware RNG values and v4 UUIDs                         |
| `ap start [ssid] [pass]`       | run the board as its own access point                    |
| `httpd start [port]`           | browse / download / upload LittleFS from a browser       |

The clock starts a background SNTP sync at boot, so `date` usually works a few
seconds after a WiFi-connected reset; set your zone once with `ntp tz 5.5` and
it is remembered across reboots.

`every` jobs run from the main loop with their output captured and echoed to
every live session, so a job started over Telnet keeps reporting on Serial too.
Commands that never return (`watch`, another `every`) are refused.

## 11. The host bridge — inspecting your laptop from the shell

The ESP32 cannot see the machine on the other end of the wire, so this is two
halves: a small agent that runs on the laptop, and the `h*` commands in the
shell that ask it questions.

### Starting it

```bash
# on the laptop (Python 3.8+, no required dependencies)
python tools/espehost.py 192.168.1.42        # or: esp32.local
```

The laptop dials **out** to the ESP32 on port 2323. That direction is
deliberate: the board's address is the stable one (mDNS, or the IP printed at
boot), a laptop's usually is not, and its inbound ports are usually firewalled.
Your laptop never opens a listening port.

Then, in the shell:

```
root@esp32:/$ host
bridge   : listening on port 2323
agent    : espehost/1.0.0 on ATLAS (Windows)
peer     : 192.168.1.101
requests : 14
caps     : cam caps cpu disk gpu io mem net notify ping power proc screen sys

root@esp32:/$ hpower
source    AC adapter
state     charging
charge    68%
power     +26.68 W  (26676 mW in)
current   2143 mA at 12.45 V
capacity  24532 / 35690 mWh

root@esp32:/$ hcam list
1 camera(s):
  [0] HP HD Camera

root@esp32:/$ hcam ascii 0 -w 60      # a frame, rendered as text
root@esp32:/$ hcam snap 0 -o shot.jpg
root@esp32:/$ hscreen                 # the display, same treatment
root@esp32:/$ hio audio               # or: usb hid keyboard mouse display serial
root@esp32:/$ hproc 10                # top processes
```

### What it can do

| Command    | Shows                                                          |
| ---------- | -------------------------------------------------------------- |
| `hsys`     | OS, build, arch, uptime                                         |
| `hcpu`     | model, physical/logical cores, per-core load, current clock     |
| `hmem`     | RAM and swap                                                    |
| `hdisk`    | mounted filesystems and free space                              |
| `hpower`   | battery %, charge state, **live current draw in mA and W**      |
| `hcam`     | `list`, `snap` to a file, `ascii` preview in the terminal       |
| `hscreen`  | the display as ASCII, or `-o file.png`                          |
| `hio`      | USB, HID, keyboards, mice, audio endpoints, displays, cameras, bluetooth, printers, disks, and `hio serial` for COM ports |
| `hnet`     | interfaces and addresses, `hnet conn` for open connections      |
| `hproc`    | top processes by CPU, `-m` by memory                            |
| `hgpu`     | graphics adapters, VRAM, current mode                           |
| `htemp`    | thermal sensors and fans, where the machine exposes them        |
| `hnotify`  | pops a desktop notification on the laptop                       |

`host caps` asks the agent what it actually supports, and `host raw <op> [args]`
sends anything through — so a capability added to the agent does **not** need a
firmware reflash.

### Observing vs. controlling

Everything above only reads the machine, and is on by default. Anything that can
*change* the laptop's state is off until you ask for it by name:

```bash
python tools/espehost.py esp32.local --allow-input   # htype, hkey, hclip
python tools/espehost.py esp32.local --allow-exec    # hexec
python tools/espehost.py esp32.local --no-screen     # opt out of hscreen
```

### Optional extras

The agent works with the standard library alone, and does more with these:

```bash
pip install psutil           # per-core CPU, real process list, sensors
pip install opencv-python    # hcam
pip install mss              # hscreen (Pillow also works)
pip install pyautogui pyperclip   # htype / hkey / hclip
```

`python tools/test_bridge.py` runs the protocol end to end against a stand-in
for the ESP32, if you want to check the agent without flashing anything.

---

## 12. Fusion mode (`fuse`) — the two machines as one system

A rule is `<source> <comparator> <threshold>` → `<any shell command>`. Because
the source can be on either side and the action is an ordinary shell line,
either machine can drive the other.

```
fuse add host.cpu gt 80 led on                 # laptop busy   -> board LED
fuse add host.power lt 20 pwm 13 255           # battery low   -> buzzer
fuse add adc.34 gt 2000 hnotify "light!"       # board sensor  -> laptop toast
fuse add pin.4 eq 1 hkey ctrl shift t          # a button      -> keystroke
fuse on
```

`fuse sources` lists what you can read: `pin.N adc.N touch.N heap temp rssi
uptime`, and `host.<op>` for anything the agent reports a number for
(`host.cpu`, `host.mem`, `host.power`, `host.disk`, ...).

Three things worth knowing:

- **Comparators are words** — `gt lt ge le eq ne`. A bare `>` never reaches the
  command; the shell's redirection parser takes it first.
- **Rules are edge-triggered.** The action runs when the condition *becomes*
  true, not on every poll while it stays true. `--level` opts into the latter.
- **Host rules cost a round-trip.** Evaluation runs on the same loop that serves
  the shell, so only one host-backed rule is checked per tick, round-robin.
  `fuse every <secs>` sets the interval (default 5s).

`fuse save` writes `/fuse.rules` in exactly the syntax `fuse add` takes, and
rules reload automatically at boot.

---

## 13. Honest limitations

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
- `mqtt` is plain MQTT only (no TLS/8883) and does not auto-reconnect after a
  drop - run `mqtt connect` again. See section 9.
- `date` has no battery-backed RTC behind it: the clock is 1970 until `ntp`
  syncs it (or `date -s` sets it), and it resets on every reboot.
- `adc` on ADC2 pins (0, 2, 4, 12-15, 25-27) is unreliable while WiFi is
  connected - the radio owns that converter. ADC1 (32-39) is always fine.
- `temp` reads the undocumented on-die sensor: it runs well above ambient, so
  use it as a trend, not a thermometer.
- `httpd` serves the whole filesystem to anyone on the network with no
  password - start it when you need it, `httpd stop` when you don't.
- `every` jobs are RAM-only and run from the main loop, so they are paced by it
  and disappear on reboot (re-add them from `/boot.sh`).
- The **host bridge port has no authentication.** Anything on your LAN can
  connect to it and pretend to be the agent, which lets it feed the board false
  readings and fire fusion rules. It cannot read your laptop — that needs a
  shell session, which is password-protected — but treat the bridge as trusted
  only on a network you trust. Point `espehost.py` at the right address, too: it
  sends whatever it is asked for to whoever answers.
- `hexec`, `htype`, `hkey` and `hclip` let the board act on the laptop. They are
  off unless the agent was started with `--allow-exec` / `--allow-input`.
- `htemp` fails on most consumer laptops: Windows rarely exposes thermal zones
  without vendor drivers. It says so rather than reporting 0 C.
- `freq` is polled rather than interrupt-driven, so it tops out around 100 kHz.
- `dht`, `sonar` and the other bit-banged sensor protocols are timing-sensitive;
  a WiFi interrupt landing mid-frame is the usual cause of a failed read, which
  is why `dht` retries.
- `bc` is double-precision throughout; `%` and the bitwise operators truncate to
  integer first, as bc does.
- `jq` reads one field (`.a.b`, `.arr[0]`) — it is not a JSON query engine.
- `rgb` needs a WS2812 wired up; the plain DevKit V1 has no addressable LED
  (`ESPE_RGB_LED_PIN` in `config.h` sets where it looks).

## 14. Project layout

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
| `mqtt_cmds.cpp`   | `mqtt` pub/sub (needs the PubSubClient library)          |
| `misc_cmds.cpp`   | archive / package-manager stubs                          |
| `esp_cmds.cpp`    | ESP32-specific commands (GPIO, PWM, sleep, OTA, dmesg, i2c, ...) |
| `gpio_cmds.cpp`   | analog / signal pins: `adc dac tone beep servo touchpin pinwatch pinout` |
| `diag_cmds.cpp`   | `temp cpufreq bench nvs mkfs neofetch`                   |
| `time_cmds.cpp`   | `date ntp cal` - SNTP clock, timezone saved in NVS       |
| `hash_cmds.cpp`   | `md5sum sha256sum crc32 base64 rand uuid`                |
| `script_cmds.cpp` | `watch repeat time seq yes true false test expr`         |
| `cron_cmds.cpp`   | `every` - repeating background jobs                      |
| `httpd_cmds.cpp`  | `httpd` - LittleFS web browser / uploader                |
| `text2_cmds.cpp`  | `comm join expand unexpand column sponge truncate bytes` |
| `calc_cmds.cpp`   | `bc factor cksum jq dd`                                  |
| `sensor_cmds.cpp` | `dht sonar pulse freq scope logic shiftout rgb spi`      |
| `espinfo_cmds.cpp`| `efuse parts psram rtcmem wdt` + small POSIX leftovers   |
| `net2_cmds.cpp`   | `nc http portscan netscan mdns`                          |
| `host_cmds.cpp`   | host bridge transport + the `h*` laptop commands         |
| `fuse_cmds.cpp`   | `fuse` - fusion-mode rule engine                         |
| `tools/espehost.py` | the laptop agent (see section 11)                      |
| `tools/test_bridge.py` | end-to-end bridge test, no hardware needed          |
| `tools/test_calc.sh`   | native unit tests for the `bc` parser               |
| `tools/build.sh`  | compile via the arduino-cli bundled with the IDE         |

New commands are added by writing a handler and appending it to that module's
`Command[]` table — the `help` listing updates automatically. A new module needs
its table declared in `shell.h` and listed in `CMD_TABLES` in `shell.cpp`.

### Tests

```bash
bash tools/build.sh        # compile (first run ~4 min, then ~30s)
bash tools/test_calc.sh    # bc expression parser, natively compiled
python tools/test_bridge.py  # host bridge protocol, end to end
```
