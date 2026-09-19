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
      struct timeval tv;
      tv.tv_sec  = local - tzOffset();
      tv.tv_usec = 0;
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

// ---- cal : a month calendar ------------------------------------------------
static bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int daysInMonth(int m, int y) {
  static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && isLeap(y)) return 29;
  return d[m - 1];
}

// Zeller's congruence: weekday of the 1st, 0 = Sunday.
static int firstWeekday(int m, int y) {
  int q = 1, mm = m, yy = y;
  if (mm < 3) { mm += 12; yy -= 1; }
  int k = yy % 100, j = yy / 100;
  int h = (q + (13 * (mm + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
  return (h + 6) % 7;   // Zeller: 0 = Saturday -> shift so 0 = Sunday
}

static int cmd_cal(int argc, char **argv, ShellIO &io) {
  static const char *MON[] = {"January", "February", "March", "April", "May", "June",
                              "July", "August", "September", "October", "November", "December"};
  int month = 0, year = 0;

  if (argc == 1) {
    if (!timeIsSet()) {
      io.out.println(F("cal: clock not set - run 'ntp', or give a month and year: cal 6 2026"));
      return 1;
    }
    time_t t = time(nullptr) + tzOffset();
    struct tm tmv;
    gmtime_r(&t, &tmv);
    month = tmv.tm_mon + 1;
    year  = tmv.tm_year + 1900;
  } else if (argc == 3) {
    month = String(argv[1]).toInt();
    year  = String(argv[2]).toInt();
  } else if (argc == 2) {
    year = String(argv[1]).toInt();
    month = 0;                     // whole year is a lot of scrolling: months 1..12
  } else {
    io.out.println(F("usage: cal [[month] year]"));
    return 1;
  }
  if (year < 1900 || year > 2100) { io.out.println(F("cal: year must be 1900 - 2100")); return 1; }

  int from = month ? month : 1, to = month ? month : 12;
  if (month && (month < 1 || month > 12)) { io.out.println(F("cal: month must be 1 - 12")); return 1; }

  for (int m = from; m <= to; ++m) {
    char head[32];
    snprintf(head, sizeof(head), "%s %d", MON[m - 1], year);
    int pad = (20 - (int)strlen(head)) / 2;
    for (int i = 0; i < pad; ++i) io.out.print(' ');
    io.out.println(head);
    io.out.println(F("Su Mo Tu We Th Fr Sa"));

    int wd = firstWeekday(m, year);
    for (int i = 0; i < wd; ++i) io.out.print(F("   "));
    for (int d = 1; d <= daysInMonth(m, year); ++d) {
      io.out.printf("%2d ", d);
      if (++wd == 7) { wd = 0; io.out.println(); }
    }
    if (wd != 0) io.out.println();
    if (m != to) io.out.println();
  }
  return 0;
}

const Command TIME_CMDS[] = {
  {"cal",  cmd_cal,  "cal [[month] year]",         "print a month calendar",      G_SYS},
  {"date", cmd_date, "date [-u] [+FMT] | -s TIME", "show / set the wall clock",   G_SYS},
  {"ntp",  cmd_ntp,  "ntp [sync [srv]|tz H|status]","sync the clock over SNTP",   G_SYS},
};
const size_t TIME_CMDS_N = sizeof(TIME_CMDS) / sizeof(TIME_CMDS[0]);
