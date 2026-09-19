#include "shell.h"
#include "config.h"
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>

// ============================================================================
//  Wall-clock time: SNTP sync, `date`, and the timezone offset (kept in NVS
//  so the board comes back up in the right zone after a reset).
//
//  Without a battery-backed RTC the ESP32 boots at 1970; the clock is only
//  meaningful after `ntp` has run (or `date -s` has set it by hand), so every
//  command here says so plainly rather than printing a fake 1970 timestamp.
// ============================================================================

static long s_tzOffsetSec = 0;      // local = UTC + this
static bool s_tzLoaded = false;

static long tzOffset() {
  if (!s_tzLoaded) {
    Preferences p;
    if (p.begin(ESPE_PREFS_NAMESPACE, true)) {
      s_tzOffsetSec = p.getLong("tzoff", 0);
      p.end();
    }
    s_tzLoaded = true;
  }
  return s_tzOffsetSec;
}

static void tzSet(long sec) {
  s_tzOffsetSec = sec;
  s_tzLoaded = true;
  Preferences p;
  if (p.begin(ESPE_PREFS_NAMESPACE, false)) { p.putLong("tzoff", sec); p.end(); }
}

// The epoch is only plausible once something has set it (SNTP or `date -s`).
bool timeIsSet() { return time(nullptr) > 1600000000; }

static String fmtTime(time_t t, const char *fmt) {
  struct tm tmv;
  gmtime_r(&t, &tmv);                       // t is already shifted by the caller
  char buf[80];
  strftime(buf, sizeof(buf), fmt, &tmv);
  return String(buf);
}

String timeNowString() {
  if (!timeIsSet()) return String("(clock not set - run 'ntp')");
  return fmtTime(time(nullptr) + tzOffset(), "%Y-%m-%d %H:%M:%S");
}

// Starts a background SNTP sync; used by `ntp` and once at boot.
void timeStartSync(const char *server) {
  configTime(0, 0, server, "pool.ntp.org", "time.nist.gov");   // keep UTC internally
}

// Called from setup() once WiFi is up: kicks off a sync without blocking boot.
void timeInitAtBoot() {
  tzOffset();                       // load the saved zone
  if (WiFi.status() == WL_CONNECTED) timeStartSync("pool.ntp.org");
}

// ---- ntp -------------------------------------------------------------------
static int cmd_ntp(int argc, char **argv, ShellIO &io) {
  String sub = (argc >= 2) ? String(argv[1]) : String("sync");

  if (sub == "status") {
    io.out.print(F("clock:  "));
    io.out.println(timeIsSet() ? F("set") : F("NOT set (run 'ntp')"));
    io.out.print(F("utc:    "));
    io.out.println(timeIsSet() ? fmtTime(time(nullptr), "%Y-%m-%d %H:%M:%S") : String("-"));
    io.out.print(F("local:  "));
    io.out.println(timeNowString());
    io.out.printf("tz:     UTC%+.2f\n", tzOffset() / 3600.0);
    return 0;
  }

  if (sub == "tz") {
    if (argc < 3) { io.out.println(F("usage: ntp tz <hours>   e.g. ntp tz 5.5")); return 1; }
    double h = String(argv[2]).toDouble();
    if (h < -12 || h > 14) { io.out.println(F("ntp: offset must be between -12 and +14 hours")); return 1; }
    tzSet((long)(h * 3600.0));
    io.out.printf("timezone set to UTC%+.2f (saved)\n", tzOffset() / 3600.0);
    io.out.print(F("local time now: ")); io.out.println(timeNowString());
    return 0;
  }

  if (sub != "sync") { io.out.println(F("usage: ntp [sync [server]] | tz <hours> | status")); return 1; }

  if (WiFi.status() != WL_CONNECTED) { io.out.println(F("ntp: WiFi not connected")); return 1; }
  const char *server = (argc >= 3) ? argv[2] : "pool.ntp.org";
  io.out.print(F("syncing with ")); io.out.print(server); io.out.println(F(" ..."));
  timeStartSync(server);

  for (int i = 0; i < 100; ++i) {          // up to ~10s, Ctrl-C-able
    if (timeIsSet()) {
      io.out.print(F("clock set: "));
      io.out.println(timeNowString());
      return 0;
    }
    if (shellWait(io, 100)) { io.out.println(F("ntp: cancelled")); return 1; }
  }
  io.out.println(F("ntp: timed out (no reply from the time server)"));
  return 1;
}

// ---- date ------------------------------------------------------------------
static int cmd_date(int argc, char **argv, ShellIO &io) {
  bool utc = false;
  const char *fmt = "%Y-%m-%d %H:%M:%S";

  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-u" || a == "--utc") { utc = true; continue; }
    if (a == "-s") {                                   // date -s "YYYY-MM-DD HH:MM:SS"
      if (i + 1 >= argc) { io.out.println(F("usage: date -s \"YYYY-MM-DD HH:MM:SS\"")); return 1; }
      String v = argv[++i];
      for (int k = i + 1; k < argc; ++k) { v += ' '; v += argv[k]; i = k; }   // unquoted date+time
      struct tm tmv;
      memset(&tmv, 0, sizeof(tmv));
      int y, mo, d, h = 0, mi = 0, se = 0;
      int got = sscanf(v.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se);
      if (got < 3) { io.out.println(F("date: expected YYYY-MM-DD [HH:MM:SS]")); return 1; }
      tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
      tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = se;
      time_t local = mktime(&tmv);                      // mktime here treats the fields as UTC
      struct timeval tv = { .tv_sec = local - tzOffset(), .tv_usec = 0 };
      settimeofday(&tv, nullptr);
      io.out.print(F("clock set: ")); io.out.println(timeNowString());
      return 0;
    }
    if (a.startsWith("+")) { fmt = argv[i] + 1; continue; }   // date +%H:%M
    io.out.println(F("usage: date [-u] [+FORMAT] | date -s \"YYYY-MM-DD HH:MM:SS\""));
    return 1;
  }

  if (!timeIsSet()) {
    io.out.println(F("date: clock not set - run 'ntp' (or 'date -s ...'); 'uptime' works regardless"));
    return 1;
  }
  io.out.println(fmtTime(time(nullptr) + (utc ? 0 : tzOffset()), fmt));
  return 0;
}

const Command TIME_CMDS[] = {
  {"date", cmd_date, "date [-u] [+FMT] | -s TIME", "show / set the wall clock",   G_SYS},
  {"ntp",  cmd_ntp,  "ntp [sync [srv]|tz H|status]","sync the clock over SNTP",   G_SYS},
};
const size_t TIME_CMDS_N = sizeof(TIME_CMDS) / sizeof(TIME_CMDS[0]);
