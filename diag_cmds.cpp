#include "shell.h"
#include "config.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_system.h"
#include "nvs.h"

// ============================================================================
//  Diagnostics: the "how is this board actually doing" commands.
// ============================================================================

// ---- temp : internal die temperature ---------------------------------------
// The classic ESP32's sensor is undocumented and reads well above ambient
// (it measures the die, next to the radio), so it is useful as a trend, not
// as a thermometer - the output says so rather than pretending otherwise.
static int cmd_temp(int argc, char **argv, ShellIO &io) {
  float c = temperatureRead();
  if (isnan(c)) { io.out.println(F("temp: no reading from the internal sensor")); return 1; }
  io.out.printf("die temperature: %.1f C  (%.1f F)\n", c, c * 9.0 / 5.0 + 32.0);
  io.out.println(F("note: this is the on-die sensor - it runs hot vs. ambient; watch the trend"));
  return 0;
}

// ---- nvs : browse the key/value store the shell persists settings in -------
// Everything ESPEShell remembers across reboots (WiFi credentials, timezone,
// boot count) lives in one NVS namespace; this is the window onto it.
static const char *nvsTypeName(nvs_type_t t) {
  switch (t) {
    case NVS_TYPE_U8: return "u8";     case NVS_TYPE_I8:  return "i8";
    case NVS_TYPE_U16: return "u16";   case NVS_TYPE_I16: return "i16";
    case NVS_TYPE_U32: return "u32";   case NVS_TYPE_I32: return "i32";
    case NVS_TYPE_U64: return "u64";   case NVS_TYPE_I64: return "i64";
    case NVS_TYPE_STR: return "str";   case NVS_TYPE_BLOB: return "blob";
    default: return "?";
  }
}

// Preferences reports its own type enum; the iterator reports the NVS one.
static nvs_type_t nvsTypeOfKey(Preferences &p, const char *key) {
  switch (p.getType(key)) {
    case PT_I8:   return NVS_TYPE_I8;
    case PT_U8:   return NVS_TYPE_U8;
    case PT_I16:  return NVS_TYPE_I16;
    case PT_U16:  return NVS_TYPE_U16;
    case PT_I32:  return NVS_TYPE_I32;
    case PT_U32:  return NVS_TYPE_U32;
    case PT_I64:  return NVS_TYPE_I64;
    case PT_U64:  return NVS_TYPE_U64;
    case PT_STR:  return NVS_TYPE_STR;
    case PT_BLOB: return NVS_TYPE_BLOB;
    default:      return NVS_TYPE_ANY;
  }
}

static void nvsPrintValue(Preferences &p, const char *key, nvs_type_t t, ShellIO &io) {
  switch (t) {
    case NVS_TYPE_STR:  io.out.println(p.getString(key, "")); break;
    case NVS_TYPE_U8:   io.out.println((long)p.getUChar(key, 0)); break;
    case NVS_TYPE_I8:   io.out.println((long)p.getChar(key, 0)); break;
    case NVS_TYPE_U16:  io.out.println((long)p.getUShort(key, 0)); break;
    case NVS_TYPE_I16:  io.out.println((long)p.getShort(key, 0)); break;
    case NVS_TYPE_U32:  io.out.println((unsigned long)p.getUInt(key, 0)); break;
    case NVS_TYPE_I32:  io.out.println((long)p.getInt(key, 0)); break;
    case NVS_TYPE_U64:  io.out.println((unsigned long long)p.getULong64(key, 0)); break;
    case NVS_TYPE_I64:  io.out.println((long long)p.getLong64(key, 0)); break;
    case NVS_TYPE_BLOB: io.out.printf("<blob, %u bytes>\n", (unsigned)p.getBytesLength(key)); break;
    default:            io.out.println(F("<unknown type>")); break;
  }
}

static int nvsList(ShellIO &io) {
  Preferences p;
  if (!p.begin(ESPE_PREFS_NAMESPACE, true)) { io.out.println(F("nvs: cannot open the namespace")); return 1; }

  io.out.print(F("namespace: "));
  io.out.println(F(ESPE_PREFS_NAMESPACE));
  io.out.println(F("KEY              TYPE  VALUE"));

  nvs_iterator_t it = nullptr;
  int n = 0;
  esp_err_t err = nvs_entry_find("nvs", ESPE_PREFS_NAMESPACE, NVS_TYPE_ANY, &it);
  while (err == ESP_OK && it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    io.out.printf("%-16s %-5s ", info.key, nvsTypeName(info.type));
    nvsPrintValue(p, info.key, info.type, io);
    n++;
    err = nvs_entry_next(&it);
  }
  if (it) nvs_release_iterator(it);
  if (n == 0) io.out.println(F("  (empty)"));
  io.out.printf("%d key(s), %u free entries in this partition\n", n, (unsigned)p.freeEntries());
  p.end();
  return 0;
}

static int cmd_nvs(int argc, char **argv, ShellIO &io) {
  String sub = (argc >= 2) ? String(argv[1]) : String("list");

  if (sub == "list" || sub == "ls") return nvsList(io);

  if (sub == "get" && argc >= 3) {
    Preferences p;
    if (!p.begin(ESPE_PREFS_NAMESPACE, true)) { io.out.println(F("nvs: cannot open the namespace")); return 1; }
    if (!p.isKey(argv[2])) { io.out.print(argv[2]); io.out.println(F(": no such key")); p.end(); return 1; }
    nvsPrintValue(p, argv[2], nvsTypeOfKey(p, argv[2]), io);
    p.end();
    return 0;
  }

  if (sub == "set" && argc >= 4) {
    String val = argv[3];
    for (int i = 4; i < argc; ++i) { val += ' '; val += argv[i]; }
    Preferences p;
    if (!p.begin(ESPE_PREFS_NAMESPACE, false)) { io.out.println(F("nvs: cannot open the namespace for writing")); return 1; }
    size_t w = p.putString(argv[2], val);
    p.end();
    if (w == 0) { io.out.println(F("nvs: write failed")); return 1; }
    io.out.printf("%s = %s (saved)\n", argv[2], val.c_str());
    return 0;
  }

  if ((sub == "rm" || sub == "remove") && argc >= 3) {
    Preferences p;
    if (!p.begin(ESPE_PREFS_NAMESPACE, false)) { io.out.println(F("nvs: cannot open the namespace for writing")); return 1; }
    bool ok = p.remove(argv[2]);
    p.end();
    if (!ok) { io.out.print(argv[2]); io.out.println(F(": no such key")); return 1; }
    io.out.printf("%s removed\n", argv[2]);
    return 0;
  }

  if (sub == "clear") {
    // This throws away saved WiFi credentials and the boot counter, so make
    // the caller say so explicitly.
    if (argc < 3 || String(argv[2]) != "--force") {
      io.out.println(F("nvs clear wipes saved WiFi credentials, the timezone and the boot count."));
      io.out.println(F("Re-run as: nvs clear --force"));
      return 1;
    }
    Preferences p;
    if (!p.begin(ESPE_PREFS_NAMESPACE, false)) { io.out.println(F("nvs: cannot open the namespace for writing")); return 1; }
    p.clear();
    p.end();
    io.out.println(F("nvs: namespace cleared"));
    return 0;
  }

  io.out.println(F("usage: nvs [list] | get <key> | set <key> <value> | rm <key> | clear --force"));
  return 1;
}

// ---- bench : quick CPU and filesystem benchmark ----------------------------
static int cmd_bench(int argc, char **argv, ShellIO &io) {
  bool doCpu = true, doFs = true;
  if (argc >= 2) {
    String a = argv[1];
    if (a == "cpu") doFs = false;
    else if (a == "fs") doCpu = false;
    else { io.out.println(F("usage: bench [cpu|fs]")); return 1; }
  }

  if (doCpu) {
    io.out.println(F("CPU:"));
    io.out.printf("  clock            %u MHz\n", (unsigned)getCpuFrequencyMhz());

    volatile uint32_t acc = 0;
    unsigned long t0 = micros();
    for (uint32_t i = 0; i < 1000000UL; ++i) acc += i ^ (i >> 3);
    unsigned long us = micros() - t0;
    io.out.printf("  1M int ops       %lu ms  (%.1f Mops/s)\n", us / 1000UL, 1000.0 / (double)us);

    volatile float f = 1.0f;
    t0 = micros();
    for (uint32_t i = 1; i <= 200000UL; ++i) f = f * 1.000001f + (float)i / 3.0f;
    us = micros() - t0;
    io.out.printf("  200k float ops   %lu ms  (%.2f Mops/s)\n", us / 1000UL, 0.2 / ((double)us / 1000000.0) / 1000.0);
  }

  if (doFs) {
    io.out.println(F("LittleFS (32 KB temp file):"));
    const size_t CHUNK = 512, TOTAL = 32768;
    uint8_t buf[CHUNK];
    for (size_t i = 0; i < CHUNK; ++i) buf[i] = (uint8_t)i;

    File f = LittleFS.open("/.bench.tmp", "w");
    if (!f) { io.out.println(F("  write: cannot create /.bench.tmp")); return 1; }
    unsigned long t0 = millis();
    size_t written = 0;
    while (written < TOTAL) {
      if (f.write(buf, CHUNK) != CHUNK) break;
      written += CHUNK;
    }
    f.flush();
    unsigned long wms = millis() - t0;
    f.close();
    io.out.printf("  write            %u KB in %lu ms  (%.1f KB/s)\n",
                  (unsigned)(written / 1024), wms, wms ? (written / 1024.0) * 1000.0 / wms : 0.0);

    f = LittleFS.open("/.bench.tmp", "r");
    if (f) {
      t0 = millis();
      size_t got = 0;
      while (true) { int n = f.read(buf, CHUNK); if (n <= 0) break; got += n; }
      unsigned long rms = millis() - t0;
      f.close();
      io.out.printf("  read             %u KB in %lu ms  (%.1f KB/s)\n",
                    (unsigned)(got / 1024), rms, rms ? (got / 1024.0) * 1000.0 / rms : 0.0);
    }
    LittleFS.remove("/.bench.tmp");
    io.out.printf("  free space       %s\n", humanBytes(LittleFS.totalBytes() - LittleFS.usedBytes()).c_str());
  }
  return 0;
}

// ---- neofetch : the whole board on one screen ------------------------------
static String uptimeText() {
  unsigned long sec = millis() / 1000UL;
  unsigned d = sec / 86400; sec %= 86400;
  unsigned h = sec / 3600;  sec %= 3600;
  unsigned m = sec / 60;
  char b[48];
  if (d) snprintf(b, sizeof(b), "%ud %uh %um", d, h, m);
  else if (h) snprintf(b, sizeof(b), "%uh %um", h, m);
  else snprintf(b, sizeof(b), "%um %lus", m, (unsigned long)(sec % 60));
  return String(b);
}

static int cmd_neofetch(int argc, char **argv, ShellIO &io) {
  // Left column: a small ESP logo. Right column: the facts.
  std::vector<String> info;
  info.push_back(g_user + "@" + g_hostname);
  info.push_back(F("-----------------"));
  info.push_back(String(F("OS:       ESPEShell ")) + ESPE_VERSION);
  info.push_back(String(F("Kernel:   Arduino-ESP32 / IDF ")) + ESP.getSdkVersion());
  info.push_back(String(F("Uptime:   ")) + uptimeText());
  info.push_back(String(F("Boots:    #")) + String(espeBootCount()));
  info.push_back(String(F("Chip:     ")) + ESP.getChipModel() + " rev" + String(ESP.getChipRevision()) +
                 ", " + String(ESP.getChipCores()) + " cores @ " + String(getCpuFrequencyMhz()) + " MHz");
  info.push_back(String(F("Flash:    ")) + humanBytes(ESP.getFlashChipSize()) + " @ " +
                 String(ESP.getFlashChipSpeed() / 1000000UL) + " MHz");
  info.push_back(String(F("Memory:   ")) + humanBytes(ESP.getHeapSize() - ESP.getFreeHeap()) + " / " +
                 humanBytes(ESP.getHeapSize()) + " used");
  info.push_back(String(F("Disk:     ")) + humanBytes(LittleFS.usedBytes()) + " / " +
                 humanBytes(LittleFS.totalBytes()) + " used (LittleFS)");
  if (WiFi.status() == WL_CONNECTED) {
    info.push_back(String(F("WiFi:     ")) + WiFi.SSID() + "  " + WiFi.RSSI() + " dBm");
    info.push_back(String(F("IP:       ")) + WiFi.localIP().toString());
  } else {
    info.push_back(String(F("WiFi:     not connected")));
  }
  info.push_back(String(F("Shell:    ")) + (g_telnetPeer.length() ? ("telnet from " + g_telnetPeer) : String("serial")));
  info.push_back(String(F("Temp:     ")) + String(temperatureRead(), 1) + " C (die)");
  info.push_back(String(F("Clock:    ")) + timeNowString());

  static const char *logo[] = {
    "   _____  ",
    "  |  ___| ",
    "  | |__   ",
    "  |  __|  ",
    "  | |___  ",
    "  |_____| ",
    "  E S P   ",
    "  3 2     ",
  };
  const size_t logoN = sizeof(logo) / sizeof(logo[0]);

  size_t rows = (info.size() > logoN) ? info.size() : logoN;
  for (size_t i = 0; i < rows; ++i) {
    io.out.print(i < logoN ? logo[i] : "          ");
    io.out.print(F("  "));
    io.out.println(i < info.size() ? info[i] : String());
  }
  return 0;
}

const Command DIAG_CMDS[] = {
  {"neofetch", cmd_neofetch, "neofetch",              "the whole board at a glance",     G_ESP},
  {"sysinfo",  cmd_neofetch, "sysinfo",               "the whole board at a glance",     G_ESP},
  {"bench", cmd_bench, "bench [cpu|fs]",             "quick CPU / filesystem benchmark", G_ESP},
  {"nvs",  cmd_nvs,  "nvs [list|get|set|rm|clear]", "browse persistent settings (NVS)", G_ESP},
  {"temp", cmd_temp, "temp", "internal die temperature", G_ESP},
};
const size_t DIAG_CMDS_N = sizeof(DIAG_CMDS) / sizeof(DIAG_CMDS[0]);
