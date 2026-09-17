#include "shell.h"
#include "config.h"
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ============================================================================
//  ESP32-specific commands: tsw, pin, restart, data, chip, heap
// ============================================================================

// ---- GPIO tracking ---------------------------------------------------------
static bool   g_pinSet[40];    // has the user configured this pin via ESPEShell?
static int8_t g_pinMode[40];   // 0=INPUT, 1=OUTPUT, 2=INPUT_PULLUP

static bool isFlash(int p) { return p >= 6 && p <= 11; }       // SPI flash pins
static bool inputOnly(int p) { return p >= 34 && p <= 39; }     // classic ESP32
static bool usablePin(int p) { return p >= 0 && p <= 39 && !isFlash(p); }

static const char *modeName(int p) {
  if (!g_pinSet[p]) return "-";
  switch (g_pinMode[p]) { case 0: return "INPUT"; case 1: return "OUTPUT"; case 2: return "PULLUP"; }
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
  g_pinSet[p] = true; g_pinMode[p] = md;
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
    pinMode(p, OUTPUT); g_pinSet[p] = true; g_pinMode[p] = 1;
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
  {"restart", cmd_restart, "restart",            "reboot the ESP32",                    G_ESP},
  {"reboot",  cmd_restart, "reboot",             "reboot the ESP32",                    G_ESP},
  {"data",    cmd_data,    "data",               "live runtime data snapshot",          G_ESP},
  {"chip",    cmd_chip,    "chip",               "chip / flash information",            G_ESP},
  {"heap",    cmd_heap,    "heap",               "one-line heap summary",               G_ESP},
};
const size_t ESP_CMDS_N = sizeof(ESP_CMDS) / sizeof(ESP_CMDS[0]);
