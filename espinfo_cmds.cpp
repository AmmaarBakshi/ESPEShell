#include "shell.h"
#include "config.h"
#include <esp_partition.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_mac.h>
#include <soc/rtc_cntl_reg.h>

// ============================================================================
//  Chip introspection, RTC memory, and the small POSIX leftovers.
// ============================================================================

// ---- chipid / efuse ---------------------------------------------------------
static int cmd_efuse(int argc, char **argv, ShellIO &io) {
  esp_chip_info_t info;
  esp_chip_info(&info);

  io.out.print(F("model    : "));  io.out.println(ESP.getChipModel());
  io.out.print(F("revision : "));  io.out.println(info.revision);
  io.out.print(F("cores    : "));  io.out.println(info.cores);

  io.out.print(F("features :"));
  if (info.features & CHIP_FEATURE_WIFI_BGN) io.out.print(F(" wifi-bgn"));
  if (info.features & CHIP_FEATURE_BT)       io.out.print(F(" bt"));
  if (info.features & CHIP_FEATURE_BLE)      io.out.print(F(" ble"));
  if (info.features & CHIP_FEATURE_EMB_FLASH) io.out.print(F(" embedded-flash"));
  if (info.features & CHIP_FEATURE_EMB_PSRAM) io.out.print(F(" embedded-psram"));
  io.out.println();

  // The chip id everyone quotes is just the low 6 bytes of the base MAC.
  uint8_t mac[6] = {0};
  esp_efuse_mac_get_default(mac);
  char buf[32];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  io.out.print(F("chip id  : "));  io.out.println(buf);

  uint32_t flashId = 0;
  if (esp_flash_read_id(nullptr, &flashId) == ESP_OK) {
    snprintf(buf, sizeof(buf), "%06X", (unsigned)(flashId & 0xFFFFFF));
    io.out.print(F("flash id : "));  io.out.print(buf);
    io.out.print(F("  (manufacturer "));
    io.out.print((unsigned)((flashId >> 16) & 0xFF), HEX);
    io.out.println(F(")"));
  }
  io.out.print(F("flash    : "));
  io.out.print(humanBytes(ESP.getFlashChipSize()));
  io.out.print(F(" at "));
  io.out.print(ESP.getFlashChipSpeed() / 1000000);
  io.out.println(F(" MHz"));
  io.out.print(F("sdk      : "));  io.out.println(ESP.getSdkVersion());
  return 0;
}

// ---- parts : the partition table --------------------------------------------
static const char *partSubtypeName(esp_partition_type_t type, uint8_t sub) {
  if (type == ESP_PARTITION_TYPE_APP) {
    if (sub == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "factory";
    if (sub == ESP_PARTITION_SUBTYPE_APP_TEST)    return "test";
    if (sub >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
        sub <  ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 16) return "ota";
    return "app";
  }
  switch (sub) {
    case ESP_PARTITION_SUBTYPE_DATA_OTA:      return "otadata";
    case ESP_PARTITION_SUBTYPE_DATA_NVS:      return "nvs";
    case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "coredump";
    case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "nvskeys";
    case ESP_PARTITION_SUBTYPE_DATA_SPIFFS:   return "spiffs";
    case ESP_PARTITION_SUBTYPE_DATA_PHY:      return "phy";
    default:                                  return "data";
  }
}

static void partsList(ShellIO &io, esp_partition_type_t type) {
  esp_partition_iterator_t it =
      esp_partition_find(type, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it) {
    const esp_partition_t *p = esp_partition_get(it);
    char line[80];
    snprintf(line, sizeof(line), "%-10s %-4s %-9s 0x%06X %9s",
             p->label,
             type == ESP_PARTITION_TYPE_APP ? "app" : "data",
             partSubtypeName(type, p->subtype),
             (unsigned)p->address,
             humanBytes(p->size).c_str());
    io.out.println(line);
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
}

static int cmd_parts(int argc, char **argv, ShellIO &io) {
  io.out.println(F("LABEL      TYPE SUBTYPE     OFFSET      SIZE"));
  partsList(io, ESP_PARTITION_TYPE_APP);
  partsList(io, ESP_PARTITION_TYPE_DATA);

  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running) {
    io.out.println();
    io.out.print(F("running from: "));
    io.out.println(running->label);
  }
  return 0;
}

// ---- psram -------------------------------------------------------------------
static int cmd_psram(int argc, char **argv, ShellIO &io) {
  const size_t total = ESP.getPsramSize();
  if (total == 0) {
    io.out.println(F("no PSRAM on this board"));
    io.out.println(F("(the plain DevKit V1 / WROOM-32 has none - WROVER modules do)"));
    return 1;
  }
  const size_t freeNow = ESP.getFreePsram();
  io.out.print(F("total : ")); io.out.println(humanBytes(total));
  io.out.print(F("used  : ")); io.out.println(humanBytes(total - freeNow));
  io.out.print(F("free  : ")); io.out.println(humanBytes(freeNow));
  io.out.print(F("max   : ")); io.out.println(humanBytes(ESP.getMaxAllocPsram()));
  return 0;
}

// ---- rtcmem : the scratch memory that survives deep sleep ---------------------
// RTC slow memory keeps its contents across deep sleep (and a soft reset), but
// not across a power cycle or the reset button. That is exactly the niche NVS
// does not fill: a counter you bump every wake without wearing out flash.
#define RTCMEM_SLOTS 8
#define RTCMEM_MAGIC 0x45535045UL   // "ESPE"

RTC_DATA_ATTR static uint32_t s_rtcMagic;
RTC_DATA_ATTR static int32_t  s_rtcSlots[RTCMEM_SLOTS];

static void rtcMemInit() {
  if (s_rtcMagic == RTCMEM_MAGIC) return;
  s_rtcMagic = RTCMEM_MAGIC;
  for (int i = 0; i < RTCMEM_SLOTS; ++i) s_rtcSlots[i] = 0;
}

static int cmd_rtcmem(int argc, char **argv, ShellIO &io) {
  rtcMemInit();

  if (argc < 2 || strcmp(argv[1], "list") == 0) {
    io.out.println(F("RTC slow memory - survives deep sleep, lost on power-off"));
    for (int i = 0; i < RTCMEM_SLOTS; ++i)
      io.out.printf("  [%d] %ld\n", i, (long)s_rtcSlots[i]);
    return 0;
  }
  if (strcmp(argv[1], "clear") == 0) {
    for (int i = 0; i < RTCMEM_SLOTS; ++i) s_rtcSlots[i] = 0;
    io.out.println(F("rtcmem: cleared"));
    return 0;
  }

  const int slot = atoi(argv[1]);
  if (slot < 0 || slot >= RTCMEM_SLOTS) {
    io.out.printf("rtcmem: slot must be 0-%d\n", RTCMEM_SLOTS - 1);
    return 1;
  }
  if (argc == 2) { io.out.println((long)s_rtcSlots[slot]); return 0; }
  if (strcmp(argv[2], "++") == 0) {
    io.out.println((long)++s_rtcSlots[slot]);
    return 0;
  }
  s_rtcSlots[slot] = atol(argv[2]);
  io.out.printf("rtcmem[%d] = %ld\n", slot, (long)s_rtcSlots[slot]);
  return 0;
}

// ---- wdt -----------------------------------------------------------------------
static int cmd_wdt(int argc, char **argv, ShellIO &io) {
  if (argc >= 2 && strcmp(argv[1], "panic") == 0) {
    // Deliberately hang so the watchdog fires - the only honest way to check
    // that it is actually armed. Guarded because it reboots the board.
    bool forced = false;
    for (int i = 2; i < argc; ++i) if (strcmp(argv[i], "--force") == 0) forced = true;
    if (!forced) {
      io.out.println(F("wdt panic: this hangs the CPU until the watchdog reboots the board."));
      io.out.println(F("           re-run as 'wdt panic --force' if that is what you want."));
      return 1;
    }
    io.out.println(F("hanging the loop task - the board should reset shortly..."));
    io.out.flush();
    for (;;) { }        // no yield, no delay: starve the watchdog
  }

  io.out.print(F("reset reason : "));
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  io.out.println(F("power-on"));               break;
    case ESP_RST_SW:       io.out.println(F("software restart"));       break;
    case ESP_RST_PANIC:    io.out.println(F("panic / exception"));      break;
    case ESP_RST_INT_WDT:  io.out.println(F("interrupt watchdog"));     break;
    case ESP_RST_TASK_WDT: io.out.println(F("task watchdog"));          break;
    case ESP_RST_WDT:      io.out.println(F("other watchdog"));         break;
    case ESP_RST_DEEPSLEEP:io.out.println(F("wake from deep sleep"));   break;
    case ESP_RST_BROWNOUT: io.out.println(F("brownout (check the supply)")); break;
    case ESP_RST_EXT:      io.out.println(F("external reset pin"));     break;
    default:               io.out.println(F("unknown"));                break;
  }
  io.out.print(F("loop task    : "));
  io.out.println(esp_task_wdt_status(nullptr) == ESP_OK
                 ? F("subscribed to the task watchdog")
                 : F("not subscribed (the Arduino loop usually is not)"));
  io.out.println(F("'wdt panic --force' hangs the CPU to prove the watchdog works"));
  return 0;
}

// ---- the small POSIX leftovers --------------------------------------------------
static int cmd_nproc(int argc, char **argv, ShellIO &io) {
  esp_chip_info_t info;
  esp_chip_info(&info);
  io.out.println(info.cores);
  return 0;
}

static int cmd_arch(int argc, char **argv, ShellIO &io) {
  io.out.println(ESP.getChipModel());
  return 0;
}

static int cmd_printenv(int argc, char **argv, ShellIO &io) {
  if (argc >= 2) {
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
      String v = envGet(argv[i]);
      if (v.length()) io.out.println(v);
      else rc = 1;        // printenv NAME exits non-zero when it is unset
    }
    return rc;
  }
  for (auto &kv : envAll()) {
    io.out.print(kv.first);
    io.out.print('=');
    io.out.println(kv.second);
  }
  return 0;
}

static int cmd_logname(int argc, char **argv, ShellIO &io) {
  io.out.println(g_user);
  return 0;
}

static int cmd_tty(int argc, char **argv, ShellIO &io) {
  // Which transport this session arrived on. Telnet sessions have a peer.
  if (g_telnetPeer.length()) {
    io.out.print(F("/dev/telnet  ("));
    io.out.print(g_telnetPeer);
    io.out.println(')');
  } else {
    io.out.println(F("/dev/serial0  (USB UART, 115200)"));
  }
  return 0;
}

static int cmd_sync(int argc, char **argv, ShellIO &io) {
  // LittleFS commits on close, so there is nothing buffered to push. Say so
  // rather than pretending to flush something.
  io.out.println(F("sync: LittleFS writes are committed on close - nothing pending"));
  return 0;
}

const Command ESPINFO_CMDS[] = {
  {"efuse",    cmd_efuse,    "efuse",              "chip identity, features, flash id",   G_ESP},
  {"chipid",   cmd_efuse,    "chipid",             "chip identity (alias of efuse)",      G_ESP},
  {"parts",    cmd_parts,    "parts",              "flash partition table",               G_ESP},
  {"psram",    cmd_psram,    "psram",              "PSRAM size and usage",                G_ESP},
  {"rtcmem",   cmd_rtcmem,   "rtcmem [N [v|++]]",  "scratch memory that survives sleep",  G_ESP},
  {"wdt",      cmd_wdt,      "wdt [panic --force]","watchdog and reset-reason status",    G_ESP},
  {"nproc",    cmd_nproc,    "nproc",              "number of CPU cores",                 G_SYS},
  {"arch",     cmd_arch,     "arch",               "chip architecture",                   G_SYS},
  {"printenv", cmd_printenv, "printenv [NAME...]", "print environment variables",         G_ENV},
  {"logname",  cmd_logname,  "logname",            "print the login name",                G_SYS},
  {"tty",      cmd_tty,      "tty",                "which transport this session uses",   G_SYS},
  {"sync",     cmd_sync,     "sync",               "flush filesystem writes",             G_FS},
};
const size_t ESPINFO_CMDS_N = sizeof(ESPINFO_CMDS) / sizeof(ESPINFO_CMDS[0]);
