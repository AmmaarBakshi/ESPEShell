#include "shell.h"
#include "config.h"
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ============================================================================
//  System & info commands, plus process/job stubs
// ============================================================================

static String uptimeStr() {
  unsigned long ms = millis();
  unsigned long s = ms / 1000;
  unsigned d = s / 86400; s %= 86400;
  unsigned h = s / 3600;  s %= 3600;
  unsigned m = s / 60;    s %= 60;
  char b[48];
  snprintf(b, sizeof(b), "%u day%s, %02u:%02u:%02u", d, d == 1 ? "" : "s", h, m, (unsigned)s);
  return String(b);
}

// ---- uname -----------------------------------------------------------------
static int cmd_uname(int argc, char **argv, ShellIO &io) {
  bool a = false, s = false, n = false, r = false, m = false, v = false;
  for (int i = 1; i < argc; ++i)
    for (const char *p = argv[i] + 1; argv[i][0] == '-' && *p; ++p)
      switch (*p) { case 'a': a = true; break; case 's': s = true; break; case 'n': n = true; break;
                    case 'r': r = true; break; case 'm': m = true; break; case 'v': v = true; break; }
  if (!(a || s || n || r || m || v)) s = true;
  String out;
  auto add = [&](const String &x) { if (out.length()) out += ' '; out += x; };
  if (a || s) add("ESPEShell");
  if (a || n) add(g_hostname);
  if (a || r) add(ESPE_VERSION);
  if (a || v) add(String("SDK-") + ESP.getSdkVersion());
  if (a || m) add(ESP.getChipModel());
  io.out.println(out);
  return 0;
}

// ---- hostname / hostnamectl ------------------------------------------------
static int cmd_hostname(int argc, char **argv, ShellIO &io) {
  if (argc >= 2) {
    g_hostname = argv[1];
    WiFi.setHostname(g_hostname.c_str());
    return 0;
  }
  io.out.println(g_hostname);
  return 0;
}

static int cmd_hostnamectl(int argc, char **argv, ShellIO &io) {
  io.out.print(F("   Static hostname: ")); io.out.println(g_hostname);
  io.out.print(F("     Chip / cores: ")); io.out.print(ESP.getChipModel());
  io.out.print(F(" x")); io.out.println(ESP.getChipCores());
  io.out.print(F("       CPU (MHz): ")); io.out.println(ESP.getCpuFreqMHz());
  io.out.print(F("      SDK version: ")); io.out.println(ESP.getSdkVersion());
  io.out.print(F("  Operating System: ESPEShell ")); io.out.println(ESPE_VERSION);
  io.out.print(F("        Chip rev.: ")); io.out.println(ESP.getChipRevision());
  return 0;
}

// ---- uptime ----------------------------------------------------------------
static int cmd_uptime(int argc, char **argv, ShellIO &io) {
  io.out.print(F(" up "));
  io.out.print(uptimeStr());
  io.out.print(F(",  tasks: "));
  io.out.print((unsigned)uxTaskGetNumberOfTasks());
  io.out.print(F(",  free heap: "));
  io.out.println(humanBytes(ESP.getFreeHeap()));
  return 0;
}

// ---- free ------------------------------------------------------------------
static int cmd_free(int argc, char **argv, ShellIO &io) {
  bool human = false;
  for (int i = 1; i < argc; ++i) if (String(argv[i]) == "-h") human = true;
  uint32_t total = ESP.getHeapSize();
  uint32_t freeh = ESP.getFreeHeap();
  uint32_t minf = ESP.getMinFreeHeap();
  uint32_t maxb = ESP.getMaxAllocHeap();
  io.out.println(F("           total       free   min-free   max-block"));
  if (human)
    io.out.printf("Heap:  %9s %9s %9s %9s\n", humanBytes(total).c_str(), humanBytes(freeh).c_str(),
                  humanBytes(minf).c_str(), humanBytes(maxb).c_str());
  else
    io.out.printf("Heap:  %9lu %9lu %9lu %9lu\n", (unsigned long)total, (unsigned long)freeh,
                  (unsigned long)minf, (unsigned long)maxb);
  uint32_t ps = ESP.getPsramSize();
  if (ps > 0) {
    uint32_t fps = ESP.getFreePsram();
    if (human) io.out.printf("PSRAM: %9s %9s\n", humanBytes(ps).c_str(), humanBytes(fps).c_str());
    else io.out.printf("PSRAM: %9lu %9lu\n", (unsigned long)ps, (unsigned long)fps);
  }
  return 0;
}

// ---- whoami / id -----------------------------------------------------------
static int cmd_whoami(int argc, char **argv, ShellIO &io) { io.out.println(g_user); return 0; }
static int cmd_id(int argc, char **argv, ShellIO &io) {
  io.out.print(F("uid=0(")); io.out.print(g_user);
  io.out.print(F(") gid=0(")); io.out.print(g_user);
  io.out.println(F(") groups=0(root)"));
  return 0;
}

// ---- who / w ---------------------------------------------------------------
static void sessions(ShellIO &io, bool wide) {
  io.out.print(g_user); io.out.print(F("   console   (serial)"));
  io.out.println();
  if (g_telnetPeer.length()) {
    io.out.print(g_user); io.out.print(F("   telnet    (")); io.out.print(g_telnetPeer); io.out.println(F(")"));
  }
}
static int cmd_who(int argc, char **argv, ShellIO &io) { sessions(io, false); return 0; }
static int cmd_w(int argc, char **argv, ShellIO &io) {
  io.out.print(F(" ")); io.out.print(uptimeStr());
  io.out.print(F(",  ")); io.out.print(g_telnetPeer.length() ? 2 : 1); io.out.println(F(" user(s)"));
  io.out.println(F("USER     TTY       FROM"));
  sessions(io, true);
  return 0;
}

// ---- passwd ----------------------------------------------------------------
static int cmd_passwd(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: passwd <new-password>  (applies until reboot)")); return 1; }
  g_telnetPassword = argv[1];
  io.out.println(F("passwd: telnet password updated (until reboot)."));
  return 0;
}

// ---- ps / top / pgrep ------------------------------------------------------
static char stateChar(eTaskState st) {
  switch (st) { case eRunning: return 'R'; case eReady: return 'r'; case eBlocked: return 'B';
                case eSuspended: return 'S'; case eDeleted: return 'D'; default: return '?'; }
}

static void taskTable(ShellIO &io) {
#if (configUSE_TRACE_FACILITY == 1)
  UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t *arr = (TaskStatus_t *)malloc(n * sizeof(TaskStatus_t));
  if (!arr) { io.out.println(F("ps: out of memory")); return; }
  uint32_t total;
  UBaseType_t got = uxTaskGetSystemState(arr, n, &total);
  io.out.println(F("  PID  S  PRI  STACK  NAME"));
  for (UBaseType_t i = 0; i < got; ++i)
    io.out.printf("%5u  %c  %3u  %5u  %s\n", (unsigned)arr[i].xTaskNumber, stateChar(arr[i].eCurrentState),
                  (unsigned)arr[i].uxCurrentPriority, (unsigned)arr[i].usStackHighWaterMark, arr[i].pcTaskName);
  free(arr);
#else
  io.out.print(F("tasks running: "));
  io.out.println((unsigned)uxTaskGetNumberOfTasks());
  io.out.println(F("(detailed task list needs configUSE_TRACE_FACILITY)"));
#endif
}

static int cmd_ps(int argc, char **argv, ShellIO &io) { taskTable(io); return 0; }

static int cmd_top(int argc, char **argv, ShellIO &io) {
  io.out.print(F("top - up ")); io.out.print(uptimeStr());
  io.out.print(F(",  tasks: ")); io.out.println((unsigned)uxTaskGetNumberOfTasks());
  io.out.printf("Heap: %s free / %s total    CPU: %u MHz x%u\n",
                humanBytes(ESP.getFreeHeap()).c_str(), humanBytes(ESP.getHeapSize()).c_str(),
                (unsigned)ESP.getCpuFreqMHz(), (unsigned)ESP.getChipCores());
  io.out.println(F("(snapshot - a line shell cannot refresh live)"));
  taskTable(io);
  return 0;
}

static int cmd_pgrep(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: pgrep name")); return 1; }
#if (configUSE_TRACE_FACILITY == 1)
  String needle = argv[1]; needle.toLowerCase();
  UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t *arr = (TaskStatus_t *)malloc(n * sizeof(TaskStatus_t));
  if (!arr) return 1;
  uint32_t total;
  UBaseType_t got = uxTaskGetSystemState(arr, n, &total);
  int rc = 1;
  for (UBaseType_t i = 0; i < got; ++i) {
    String nm = arr[i].pcTaskName; nm.toLowerCase();
    if (nm.indexOf(needle) >= 0) { io.out.printf("%u %s\n", (unsigned)arr[i].xTaskNumber, arr[i].pcTaskName); rc = 0; }
  }
  free(arr);
  return rc;
#else
  io.out.println(F("pgrep: task introspection disabled in this build"));
  return 1;
#endif
}

// ---- process / job stubs ---------------------------------------------------
static int cmd_noproc(int argc, char **argv, ShellIO &io) {
  io.out.print(argv[0]);
  io.out.println(F(": no user processes on ESP32 (see 'ps' for FreeRTOS tasks)"));
  return 1;
}
static int cmd_jobs(int argc, char **argv, ShellIO &io) { io.out.println(F("no jobs")); return 0; }
static int cmd_nohup(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: nohup cmd [args]")); return 1; }
  String line;
  for (int i = 1; i < argc; ++i) { line += argv[i]; line += ' '; }
  return runLine(line, io.out);
}

const Command SYS_CMDS[] = {
  {"uname",       cmd_uname,       "uname [-asnrmv]",  "system information",              G_SYS},
  {"hostname",    cmd_hostname,    "hostname [name]",  "show / set the hostname",         G_SYS},
  {"hostnamectl", cmd_hostnamectl, "hostnamectl",      "detailed host / chip info",       G_SYS},
  {"uptime",      cmd_uptime,      "uptime",           "how long the system has run",     G_SYS},
  {"free",        cmd_free,        "free [-h]",        "memory (heap / PSRAM) usage",     G_SYS},
  {"whoami",      cmd_whoami,      "whoami",           "print the current user",          G_SYS},
  {"id",          cmd_id,          "id",               "print user / group ids",          G_SYS},
  {"who",         cmd_who,         "who",              "who is logged on",                G_SYS},
  {"w",           cmd_w,           "w",                "who is on + uptime",              G_SYS},
  {"passwd",      cmd_passwd,      "passwd <new>",     "change the telnet password",      G_SYS},
  {"ps",          cmd_ps,          "ps",               "list FreeRTOS tasks",             G_PROC},
  {"top",         cmd_top,         "top",              "task/heap snapshot",              G_PROC},
  {"htop",        cmd_top,         "htop",             "task/heap snapshot",              G_PROC},
  {"pgrep",       cmd_pgrep,       "pgrep name",       "find tasks by name",              G_PROC},
  {"kill",        cmd_noproc,      "kill pid",         "(no user processes)",             G_PROC},
  {"killall",     cmd_noproc,      "killall name",     "(no user processes)",             G_PROC},
  {"pkill",       cmd_noproc,      "pkill name",       "(no user processes)",             G_PROC},
  {"bg",          cmd_noproc,      "bg",               "(no job control)",                G_PROC},
  {"fg",          cmd_noproc,      "fg",               "(no job control)",                G_PROC},
  {"jobs",        cmd_jobs,        "jobs",             "list jobs (none)",                G_PROC},
  {"nohup",       cmd_nohup,       "nohup cmd...",     "run a command (hangup n/a)",      G_PROC},
};
const size_t SYS_CMDS_N = sizeof(SYS_CMDS) / sizeof(SYS_CMDS[0]);
