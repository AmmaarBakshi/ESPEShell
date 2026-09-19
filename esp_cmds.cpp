#include "shell.h"
#include "config.h"
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <Wire.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include "esp_system.h"
#if __has_include("esp_arduino_version.h")
#include "esp_arduino_version.h"
#endif

// ============================================================================
//  ESP32-specific commands: tsw, pin, restart, data, chip, heap
// ============================================================================

// ---- GPIO tracking ---------------------------------------------------------
// One registry for every command that grabs a pin (pin, pwm, led, blink, and
// the peripheral commands in gpio_cmds.cpp) so `pin --used` / `--free` always
// tell the truth about what the shell has claimed.
static bool   g_pinSet[40];    // has the user configured this pin via ESPEShell?
static int8_t g_pinMode[40];   // see PIN_* in shell.h

bool espePinUsable(int p) { return p >= 0 && p <= 39 && !(p >= 6 && p <= 11); }
bool espePinInputOnly(int p) { return p >= 34 && p <= 39; }     // classic ESP32

void espeMarkPin(int p, int mode) {
  if (p < 0 || p > 39) return;
  g_pinSet[p] = true;
  g_pinMode[p] = (int8_t)mode;
}

void espeReleasePin(int p) {
  if (p < 0 || p > 39) return;
  g_pinSet[p] = false;
  g_pinMode[p] = 0;
}

static bool isFlash(int p) { return p >= 6 && p <= 11; }       // SPI flash pins
static bool inputOnly(int p) { return espePinInputOnly(p); }
static bool usablePin(int p) { return espePinUsable(p); }

static const char *modeName(int p) {
  if (!g_pinSet[p]) return "-";
  switch (g_pinMode[p]) {
    case PIN_INPUT:  return "INPUT";
    case PIN_OUTPUT: return "OUTPUT";
    case PIN_PULLUP: return "PULLUP";
    case PIN_PWM:    return "PWM";
    case PIN_ADC:    return "ADC";
    case PIN_DAC:    return "DAC";
    case PIN_TOUCH:  return "TOUCH";
    case PIN_TONE:   return "TONE";
    case PIN_SERVO:  return "SERVO";
  }
  return "?";
}

static void pinRow(ShellIO &io, int p) {
  io.out.printf("  GPIO%-2d  %-7s  val=%d", p, modeName(p), digitalRead(p));
  if (inputOnly(p)) io.out.print(F("  (input-only)"));
  if (p == 0 || p == 2 || p == 5 || p == 12 || p == 15) io.out.print(F("  (strapping)"));
  io.out.println();
}

static String uptimeShort() {
  unsigned long s = millis() / 1000;
  unsigned d = s / 86400; s %= 86400;
  unsigned h = s / 3600;  s %= 3600;
  unsigned m = s / 60;    s %= 60;
  char b[48];
  snprintf(b, sizeof(b), "%ud %02uh %02um %02us", d, h, m, (unsigned)s);
  return String(b);
}

// ---- tsw : time since wake -------------------------------------------------
static int cmd_tsw(int argc, char **argv, ShellIO &io) {
  io.out.print(F("awake for "));
  io.out.print(uptimeShort());
  io.out.printf("   (%lu ms)\n", millis());
  return 0;
}

// ---- pin -------------------------------------------------------------------
static int applyMode(int p, const String &m, ShellIO &io) {
  if (!usablePin(p)) { io.out.printf("pin: GPIO%d is not usable\n", p); return 1; }
  int md;
  if (m == "in" || m == "input") md = 0;
  else if (m == "out" || m == "output") { if (inputOnly(p)) { io.out.printf("pin: GPIO%d is input-only\n", p); return 1; } md = 1; }
  else if (m == "up" || m == "pullup") md = 2;
  else { io.out.println(F("pin: mode is in|out|up")); return 1; }
  pinMode(p, md == 0 ? INPUT : md == 1 ? OUTPUT : INPUT_PULLUP);
  espeMarkPin(p, md);
  io.out.printf("GPIO%d -> %s\n", p, modeName(p));
  return 0;
}

static int cmd_pin(int argc, char **argv, ShellIO &io) {
  String a1 = argc >= 2 ? String(argv[1]) : String("--status");

  if (a1 == "--status" || a1 == "-s") {
    io.out.println(F("Usable GPIO pins:"));
    for (int p = 0; p <= 39; ++p) if (usablePin(p)) pinRow(io, p);
    return 0;
  }
  if (a1 == "--all" || a1 == "-a") {
    io.out.println(F("All GPIO 0..39:"));
    for (int p = 0; p <= 39; ++p) {
      if (isFlash(p)) { io.out.printf("  GPIO%-2d  (SPI flash - do not use)\n", p); continue; }
      pinRow(io, p);
    }
    return 0;
  }
  if (a1 == "--used" || a1 == "-u") {
    io.out.println(F("Pins configured by ESPEShell:"));
    bool any = false;
    for (int p = 0; p <= 39; ++p) if (usablePin(p) && g_pinSet[p]) { pinRow(io, p); any = true; }
    if (!any) io.out.println(F("  (none yet)"));
    return 0;
  }
  if (a1 == "--free" || a1 == "-f") {
    io.out.println(F("Unconfigured usable pins:"));
    for (int p = 0; p <= 39; ++p) if (usablePin(p) && !g_pinSet[p]) io.out.printf("  GPIO%d\n", p);
    return 0;
  }
  if (a1 == "mode" && argc >= 4) return applyMode(atoi(argv[2]), String(argv[3]), io);
  if (a1 == "read" && argc >= 3) { int p = atoi(argv[2]); io.out.printf("GPIO%d = %d\n", p, digitalRead(p)); return 0; }
  if (a1 == "aread" && argc >= 3) { int p = atoi(argv[2]); io.out.printf("GPIO%d analog = %d\n", p, analogRead(p)); return 0; }
  if (a1 == "write" && argc >= 4) {
    int p = atoi(argv[2]); int v = atoi(argv[3]) ? HIGH : LOW;
    if (!usablePin(p) || inputOnly(p)) { io.out.printf("pin: GPIO%d cannot be an output\n", p); return 1; }
    pinMode(p, OUTPUT); espeMarkPin(p, PIN_OUTPUT);
    digitalWrite(p, v);
    io.out.printf("GPIO%d <- %d\n", p, v ? 1 : 0);
    return 0;
  }
  // bare number: read it
  if (isdigit((int)a1[0])) { int p = a1.toInt(); io.out.printf("GPIO%d = %d\n", p, digitalRead(p)); return 0; }

  io.out.println(F("usage: pin [--status|--all|--used|--free]"));
  io.out.println(F("       pin mode <n> <in|out|up> | pin read <n> | pin aread <n> | pin write <n> <0|1>"));
  return 1;
}

// ---- pwm --------------------------------------------------------------
// The ESP32 Arduino core's LEDC API changed between core 2.x (explicit
// channel: ledcSetup+ledcAttachPin+ledcWrite(channel)) and core 3.x
// (pin-based: ledcAttach+ledcWrite(pin)). Detect via the core version so
// this builds either way.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#define ESPE_LEDC_NEW_API 1
#else
#define ESPE_LEDC_NEW_API 0
#endif

static bool     g_pwmSet[40];
static uint32_t g_pwmFreq[40];
#if !ESPE_LEDC_NEW_API
static int8_t g_pwmChan[40];
static int8_t g_pwmNextChan = 0;
#endif

static int cmd_pwm(int argc, char **argv, ShellIO &io) {
  if (argc < 2) {
    io.out.println(F("usage: pwm <pin> <duty 0-255> [freqHz] | pwm <pin> off | pwm --status"));
    return 1;
  }
  String a1 = argv[1];
  if (a1 == "--status" || a1 == "-s") {
    io.out.println(F("PWM-configured pins:"));
    bool any = false;
    for (int p = 0; p <= 39; ++p)
      if (g_pwmSet[p]) { io.out.printf("  GPIO%-2d  freq=%luHz  res=8bit\n", p, (unsigned long)g_pwmFreq[p]); any = true; }
    if (!any) io.out.println(F("  (none yet)"));
    return 0;
  }

  int p = a1.toInt();
  if (!usablePin(p) || inputOnly(p)) { io.out.printf("pwm: GPIO%d cannot output\n", p); return 1; }

  if (argc >= 3 && String(argv[2]) == "off") {
    if (g_pwmSet[p]) {
#if ESPE_LEDC_NEW_API
      ledcDetach(p);
#else
      ledcDetachPin(p);
#endif
      g_pwmSet[p] = false;
      g_pinSet[p] = false;   // no longer claimed (keeps `pin --used` honest)
    }
    io.out.printf("GPIO%d: PWM off\n", p);
    return 0;
  }

  if (argc < 3) { io.out.println(F("usage: pwm <pin> <duty 0-255> [freqHz]")); return 1; }
  int duty = atoi(argv[2]);
  if (duty < 0) duty = 0;
  if (duty > 255) duty = 255;
  uint32_t freq = (argc >= 4) ? (uint32_t)atol(argv[3]) : 5000;

  if (!g_pwmSet[p]) {
#if ESPE_LEDC_NEW_API
    if (!ledcAttach(p, freq, 8)) { io.out.printf("pwm: failed to attach GPIO%d\n", p); return 1; }
#else
    if (g_pwmNextChan > 15) { io.out.println(F("pwm: all 16 LEDC channels are in use")); return 1; }
    int ch = g_pwmNextChan++;
    ledcSetup(ch, freq, 8);
    ledcAttachPin(p, ch);
    g_pwmChan[p] = ch;
#endif
    g_pwmSet[p] = true;
    espeMarkPin(p, PIN_PWM);   // show up in `pin --used` as PWM
  } else if (argc >= 4) {
#if ESPE_LEDC_NEW_API
    ledcChangeFrequency(p, freq, 8);
#else
    ledcSetup(g_pwmChan[p], freq, 8);
#endif
  }
  g_pwmFreq[p] = freq;

#if ESPE_LEDC_NEW_API
  ledcWrite(p, duty);
#else
  ledcWrite(g_pwmChan[p], duty);
#endif
  io.out.printf("GPIO%d: PWM duty=%d/255 freq=%luHz\n", p, duty, (unsigned long)freq);
  return 0;
}

// ---- i2cscan ------------------------------------------------------------
static int cmd_i2cscan(int argc, char **argv, ShellIO &io) {
  int sda = -1, scl = -1;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-sda" && i + 1 < argc) sda = atoi(argv[++i]);
    else if (a == "-scl" && i + 1 < argc) scl = atoi(argv[++i]);
  }
  if (sda >= 0 && scl >= 0) Wire.begin(sda, scl);
  else Wire.begin();  // board-default SDA/SCL pins

  io.out.print(F("Scanning I2C bus"));
  if (sda >= 0 && scl >= 0) io.out.printf(" (SDA=%d SCL=%d)", sda, scl);
  io.out.println(F(" ..."));

  int found = 0;
  for (uint8_t addr = 1; addr < 127; ++addr) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) { io.out.printf("  found device at 0x%02X\n", addr); found++; }
  }
  if (!found) io.out.println(F("  no devices found"));
  else io.out.printf("%d device(s) found\n", found);
  return found ? 0 : 1;
}

// ---- wifiscan -------------------------------------------------------------
static const char *encName(wifi_auth_mode_t enc) {
  switch (enc) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "?";
  }
}

static int cmd_wifiscan(int argc, char **argv, ShellIO &io) {
  io.out.println(F("Scanning WiFi networks (may briefly pause the current connection)..."));
  if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
  // An in-progress (re)connect attempt blocks the scan and makes it fail -
  // stop it first when we aren't actually associated to anything.
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect(false, false);
  WiFi.scanDelete();
  int n = WiFi.scanNetworks();
  if (n < 0) {           // radio may still have been busy: one retry
    delay(400);
    WiFi.scanDelete();
    n = WiFi.scanNetworks();
  }
  if (n < 0) { io.out.printf("wifiscan: scan failed (code %d)\n", n); return 1; }
  if (n == 0) { io.out.println(F("no networks found")); return 0; }
  io.out.println(F("  RSSI  CH  ENC        SSID"));
  for (int i = 0; i < n; ++i)
    io.out.printf("  %4d  %2d  %-9s  %s\n", WiFi.RSSI(i), WiFi.channel(i),
                  encName(WiFi.encryptionType(i)), WiFi.SSID(i).c_str());
  io.out.printf("%d network(s) found\n", n);
  WiFi.scanDelete();
  return 0;
}

// ---- led : onboard LED (DevKit V1: GPIO2) ----------------------------------
static bool g_ledOn = false;
static bool g_ledInit = false;

static int cmd_led(int argc, char **argv, ShellIO &io) {
  if (!g_ledInit) {
    pinMode(ESPE_ONBOARD_LED_PIN, OUTPUT);
    g_ledInit = true;
    g_pinSet[ESPE_ONBOARD_LED_PIN] = true;   // show up in `pin --used`
    g_pinMode[ESPE_ONBOARD_LED_PIN] = 1;
  }
  String a = argc >= 2 ? String(argv[1]) : String("status");
  if (a == "on") g_ledOn = true;
  else if (a == "off") g_ledOn = false;
  else if (a == "toggle") g_ledOn = !g_ledOn;
  else if (a != "status") { io.out.println(F("usage: led on|off|toggle|status")); return 1; }
  digitalWrite(ESPE_ONBOARD_LED_PIN, g_ledOn ? HIGH : LOW);
  io.out.printf("onboard LED (GPIO%d): %s\n", ESPE_ONBOARD_LED_PIN, g_ledOn ? "on" : "off");
  return 0;
}

// ---- blink : flash the onboard LED for a while -----------------------------
static int cmd_blink(int argc, char **argv, ShellIO &io) {
  int secs = 10;   // default: blink for 10 seconds
  int rate = 1;    // default: 1 blink per second
  int seen = 0;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a.length() > 1 && a[0] == '-' && isdigit((int)a[1])) {
      long v = a.substring(1).toInt();
      if (seen == 0) secs = (int)v;
      else if (seen == 1) rate = (int)v;
      seen++;
    } else {
      io.out.println(F("usage: blink [-seconds] [-blinks-per-second]"));
      io.out.println(F("       blink        -> 10s at 1/sec"));
      io.out.println(F("       blink -20    -> 20s at 1/sec"));
      io.out.println(F("       blink -20 -2 -> 20s at 2/sec"));
      return 1;
    }
  }
  if (secs <= 0) { io.out.println(F("blink: seconds must be > 0")); return 1; }
  if (rate <= 0) { io.out.println(F("blink: blinks per second must be > 0")); return 1; }
  if (rate > 50) rate = 50;   // faster than this just looks like a solid LED

  const int pin = ESPE_ONBOARD_LED_PIN;
  pinMode(pin, OUTPUT);
  g_ledInit = true;
  espeMarkPin(pin, PIN_OUTPUT);

  int halfMs = 500 / rate;             // on for half a period, off for half
  long blinks = (long)secs * rate;
  io.out.printf("blinking GPIO%d for %ds at %d/sec (%ld blinks) - Ctrl-C to stop\n",
                pin, secs, rate, blinks);

  unsigned long start = millis();
  unsigned long durMs = (unsigned long)secs * 1000UL;
  bool aborted = false;
  while (millis() - start < durMs) {
    digitalWrite(pin, HIGH);
    if (shellWait(io, halfMs)) { aborted = true; break; }
    digitalWrite(pin, LOW);
    if (shellWait(io, halfMs)) { aborted = true; break; }
  }
  digitalWrite(pin, LOW);
  g_ledOn = false;
  io.out.println(aborted ? F("blink: stopped.") : F("blink: done."));
  return 0;
}

// ---- ota : download a .bin over HTTP(S) and flash it -----------------------
static int cmd_ota(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: ota <url-to-firmware.bin>")); return 1; }
  if (WiFi.status() != WL_CONNECTED) { io.out.println(F("ota: WiFi not connected")); return 1; }
  String url = argv[1];

  HTTPClient http;
  WiFiClientSecure *sc = nullptr;
  WiFiClient plain;
  bool began;
  if (url.startsWith("https")) {
    sc = new WiFiClientSecure();
    sc->setInsecure();
    began = http.begin(*sc, url);
  } else {
    began = http.begin(plain, url);
  }
  if (!began) { io.out.println(F("ota: could not open URL")); delete sc; return 1; }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    io.out.printf("ota: HTTP %d\n", code);
    http.end(); delete sc; return 1;
  }
  int len = http.getSize();
  if (len <= 0) {
    io.out.println(F("ota: server did not report a content length (Content-Length required)"));
    http.end(); delete sc; return 1;
  }
  io.out.printf("ota: downloading %d bytes and flashing...\n", len);

  if (!Update.begin(len)) {
    io.out.print(F("ota: not enough space: "));
    io.out.println(Update.errorString());
    http.end(); delete sc; return 1;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  bool ok = (written == (size_t)len) && Update.end(true);
  http.end();
  delete sc;

  if (!ok) {
    io.out.printf("ota: failed (%u/%d bytes written): ", (unsigned)written, len);
    io.out.println(Update.errorString());
    return 1;
  }
  io.out.println(F("ota: flashed OK. Rebooting..."));
  io.out.flush();
  delay(300);
  ESP.restart();
  return 0;  // not reached
}

// ---- sleep / deepsleep : power management -----------------------------
#include "esp_sleep.h"

// GPIOs that can wake the classic ESP32 from deep sleep (RTC IO domain).
static bool rtcGpio(int p) {
  switch (p) {
    case 0: case 2: case 4: case 12: case 13: case 14: case 15:
    case 25: case 26: case 27: case 32: case 33: case 34: case 35:
    case 36: case 37: case 38: case 39:
      return true;
    default:
      return false;
  }
}

static int cmd_sleep(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: sleep <seconds>   (light sleep - RAM kept, WiFi/telnet likely drops)")); return 1; }
  double secs = atof(argv[1]);
  if (secs <= 0) { io.out.println(F("sleep: seconds must be > 0")); return 1; }
  io.out.printf("light sleep for %.1fs (the current connection may drop)...\n", secs);
  io.out.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)(secs * 1000000ULL));
  esp_light_sleep_start();
  io.out.println(F("awake."));
  return 0;
}

static int cmd_deepsleep(int argc, char **argv, ShellIO &io) {
  if (argc < 2) {
    io.out.println(F("usage: deepsleep <seconds> [pin<N> <0|1>]"));
    io.out.println(F("       0 seconds = timer disabled, wake by pin only."));
    io.out.println(F("       deep sleep resets the chip on wake - setup() runs again."));
    return 1;
  }
  double secs = atof(argv[1]);
  if (secs < 0) { io.out.println(F("deepsleep: seconds must be >= 0")); return 1; }
  if (secs > 0) esp_sleep_enable_timer_wakeup((uint64_t)(secs * 1000000ULL));

  if (argc >= 4 && String(argv[2]).startsWith("pin")) {
    int pin = String(argv[2]).substring(3).toInt();
    int level = atoi(argv[3]) ? 1 : 0;
    if (!rtcGpio(pin)) { io.out.printf("deepsleep: GPIO%d cannot wake from deep sleep\n", pin); return 1; }
    esp_sleep_enable_ext0_wakeup((gpio_num_t)pin, level);
    io.out.printf("deep sleep: wake on timer (%.1fs) or GPIO%d == %d ...\n", secs, pin, level);
  } else {
    io.out.printf("deep sleep for %.1fs...\n", secs);
  }
  io.out.println(F("rebooting into setup() on wake."));
  io.out.flush();
  delay(200);
  esp_deep_sleep_start();
  return 0;  // not reached
}

// ---- dmesg : reset reason, sleep-wake cause, persisted boot count ---------
static uint32_t g_bootCount = 0;
static bool g_bootCountLoaded = false;

uint32_t espeBootCount() {
  if (!g_bootCountLoaded) {
    Preferences prefs;
    uint32_t n = 0;
    if (prefs.begin(ESPE_PREFS_NAMESPACE, false)) {
      n = prefs.getUInt("bootcnt", 0) + 1;
      prefs.putUInt("bootcnt", n);
      prefs.end();
    }
    g_bootCount = n;
    g_bootCountLoaded = true;
  }
  return g_bootCount;
}

static const char *resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software (restart/reboot/ota)";
    case ESP_RST_PANIC:     return "panic / exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "unknown";
  }
}

static const char *wakeCauseStr(esp_sleep_wakeup_cause_t c) {
  switch (c) {
    case ESP_SLEEP_WAKEUP_EXT0:      return "external GPIO (ext0)";
    case ESP_SLEEP_WAKEUP_EXT1:      return "external GPIO (ext1)";
    case ESP_SLEEP_WAKEUP_TIMER:     return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "touch pad";
    case ESP_SLEEP_WAKEUP_ULP:       return "ULP program";
    default:                         return "other";
  }
}

static int cmd_dmesg(int argc, char **argv, ShellIO &io) {
  io.out.println(F("=== ESPEShell dmesg ==="));
  io.out.print(F("boot count   : ")); io.out.println(espeBootCount());
  io.out.print(F("reset reason : ")); io.out.println(resetReasonStr(esp_reset_reason()));
  esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
  if (wc != ESP_SLEEP_WAKEUP_UNDEFINED) {
    io.out.print(F("wake cause   : ")); io.out.println(wakeCauseStr(wc));
  }
  io.out.print(F("uptime       : ")); io.out.println(uptimeShort());
  io.out.println(F("(reset reason + boot count only - full panic backtraces/core dumps are not decoded)"));
  return 0;
}

// ---- restart / reboot ------------------------------------------------------
static int cmd_restart(int argc, char **argv, ShellIO &io) {
  io.out.println(F("Restarting ESP32..."));
  io.out.flush();
  delay(200);
  ESP.restart();
  return 0;  // not reached
}

// ---- data : live runtime snapshot ------------------------------------------
static int cmd_data(int argc, char **argv, ShellIO &io) {
  io.out.println(F("=== ESPEShell live data ==="));
  io.out.print(F("uptime      : ")); io.out.println(uptimeShort());
  io.out.print(F("free heap   : ")); io.out.print(humanBytes(ESP.getFreeHeap()));
  io.out.print(F(" / ")); io.out.println(humanBytes(ESP.getHeapSize()));
  io.out.print(F("min heap    : ")); io.out.println(humanBytes(ESP.getMinFreeHeap()));
  io.out.print(F("cpu freq    : ")); io.out.print(ESP.getCpuFreqMHz()); io.out.println(F(" MHz"));
  io.out.print(F("wifi        : "));
  if (WiFi.status() == WL_CONNECTED) {
    io.out.print(WiFi.SSID()); io.out.print(F("  ip=")); io.out.print(WiFi.localIP());
    io.out.print(F("  rssi=")); io.out.print(WiFi.RSSI()); io.out.println(F(" dBm"));
  } else io.out.println(F("disconnected"));
  io.out.print(F("telnet peer : ")); io.out.println(g_telnetPeer.length() ? g_telnetPeer : String("(none)"));
  io.out.print(F("bytes in/out: ")); io.out.print((unsigned long)g_bytesIn);
  io.out.print(F(" / ")); io.out.println((unsigned long)g_bytesOut);
  io.out.print(F("tasks       : ")); io.out.println((unsigned)uxTaskGetNumberOfTasks());
  return 0;
}

// ---- chip ------------------------------------------------------------------
static int cmd_chip(int argc, char **argv, ShellIO &io) {
  io.out.print(F("model    : ")); io.out.println(ESP.getChipModel());
  io.out.print(F("revision : ")); io.out.println(ESP.getChipRevision());
  io.out.print(F("cores    : ")); io.out.println(ESP.getChipCores());
  io.out.print(F("cpu MHz  : ")); io.out.println(ESP.getCpuFreqMHz());
  io.out.print(F("flash    : ")); io.out.println(humanBytes(ESP.getFlashChipSize()));
  io.out.print(F("sdk      : ")); io.out.println(ESP.getSdkVersion());
  io.out.print(F("mac      : ")); io.out.println(WiFi.macAddress());
  return 0;
}

// ---- heap ------------------------------------------------------------------
static int cmd_heap(int argc, char **argv, ShellIO &io) {
  io.out.printf("free %s, min %s, largest block %s, total %s\n",
                humanBytes(ESP.getFreeHeap()).c_str(), humanBytes(ESP.getMinFreeHeap()).c_str(),
                humanBytes(ESP.getMaxAllocHeap()).c_str(), humanBytes(ESP.getHeapSize()).c_str());
  return 0;
}

const Command ESP_CMDS[] = {
  {"tsw",     cmd_tsw,     "tsw",                "time since wake (uptime)",            G_ESP},
  {"pin",     cmd_pin,     "pin [--all|mode|..]","inspect / drive GPIO pins",           G_ESP},
  {"pwm",     cmd_pwm,     "pwm <pin> <duty> [freq]|off|--status","software PWM output (LEDC)",  G_ESP},
  {"led",     cmd_led,     "led on|off|toggle|status", "onboard LED (DevKit V1: GPIO2)",G_ESP},
  {"blink",   cmd_blink,   "blink [-secs] [-per-sec]",  "flash the onboard LED",        G_ESP},
  {"ota",     cmd_ota,     "ota <url>",                 "download+flash firmware over HTTP(S)", G_ESP},
  {"sleep",     cmd_sleep,     "sleep <secs>",            "light sleep (RAM kept)",        G_ESP},
  {"deepsleep", cmd_deepsleep, "deepsleep <secs> [pinN v]","deep sleep (resets on wake)",  G_ESP},
  {"dmesg",     cmd_dmesg,     "dmesg",                   "reset reason, wake cause, boot count", G_ESP},
  {"i2cscan", cmd_i2cscan, "i2cscan [-sda P] [-scl P]", "scan the I2C bus for devices", G_ESP},
  {"wifiscan",cmd_wifiscan,"wifiscan",                   "list nearby WiFi networks",    G_ESP},
  {"restart", cmd_restart, "restart",            "reboot the ESP32",                    G_ESP},
  {"reboot",  cmd_restart, "reboot",             "reboot the ESP32",                    G_ESP},
  {"data",    cmd_data,    "data",               "live runtime data snapshot",          G_ESP},
  {"chip",    cmd_chip,    "chip",               "chip / flash information",            G_ESP},
  {"heap",    cmd_heap,    "heap",               "one-line heap summary",               G_ESP},
};
const size_t ESP_CMDS_N = sizeof(ESP_CMDS) / sizeof(ESP_CMDS[0]);
