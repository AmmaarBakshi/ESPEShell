#include "shell.h"
#include <vector>

// ============================================================================
//  every - repeating background jobs.
//
//  `watch` holds the session while it repeats; these jobs keep running while
//  you carry on typing. The main loop asks cronPopDue() for whatever has come
//  round, runs it once with the output captured, and echoes that to every
//  live session (see drainCron() in ESPEShell.ino).
// ============================================================================

struct CronJob {
  String        line;
  unsigned long everyMs;
  unsigned long nextAt;
  unsigned long runs;
  bool          active;
};

static std::vector<CronJob> s_jobs;

// Returns the next job whose time has come, or false when nothing is due.
bool cronPopDue(String &line) {
  unsigned long now = millis();
  for (auto &j : s_jobs) {
    if (!j.active) continue;
    if ((long)(now - j.nextAt) >= 0) {
      j.nextAt = now + j.everyMs;
      j.runs++;
      line = j.line;
      return true;
    }
  }
  return false;
}

size_t cronJobCount() {
  size_t n = 0;
  for (auto &j : s_jobs) if (j.active) n++;
  return n;
}

static int cmd_every(int argc, char **argv, ShellIO &io) {
  if (argc < 2 || String(argv[1]) == "--list" || String(argv[1]) == "-l") {
    if (s_jobs.empty()) {
      io.out.println(F("no background jobs   (every <secs> <command...>)"));
      return 0;
    }
    io.out.println(F("ID  EVERY   RUNS  NEXT IN  COMMAND"));
    for (size_t i = 0; i < s_jobs.size(); ++i) {
      CronJob &j = s_jobs[i];
      if (!j.active) continue;
      long due = (long)(j.nextAt - millis());
      if (due < 0) due = 0;
      io.out.printf("%-3u %-6lu  %-4lu  %-7lu  %s\n", (unsigned)i, j.everyMs / 1000UL,
                    j.runs, (unsigned long)(due / 1000), j.line.c_str());
    }
    return 0;
  }

  String a1 = argv[1];

  if (a1 == "--del" || a1 == "-d") {
    if (argc < 3) { io.out.println(F("usage: every --del <id>")); return 1; }
    size_t id = (size_t)String(argv[2]).toInt();
    if (id >= s_jobs.size() || !s_jobs[id].active) { io.out.println(F("every: no such job")); return 1; }
    s_jobs[id].active = false;
    io.out.printf("every: job %u removed\n", (unsigned)id);
    return 0;
  }

  if (a1 == "--clear" || a1 == "-c") {
    size_t n = cronJobCount();
    s_jobs.clear();
    io.out.printf("every: %u job(s) removed\n", (unsigned)n);
    return 0;
  }

  long secs = a1.toInt();
  if (secs < 1) { io.out.println(F("usage: every <secs> <command...> | --list | --del <id> | --clear")); return 1; }
  if (argc < 3) { io.out.println(F("every: no command given")); return 1; }
  if (cronJobCount() >= 8) { io.out.println(F("every: too many jobs (8 max) - remove one first")); return 1; }

  String line;
  for (int i = 2; i < argc; ++i) { if (line.length()) line += ' '; line += argv[i]; }

  // Refuse the obvious foot-gun: a job that blocks the loop forever.
  if (line.startsWith("watch ") || line.startsWith("every ")) {
    io.out.println(F("every: that command would never finish - pick a one-shot command"));
    return 1;
  }

  CronJob j;
  j.line    = line;
  j.everyMs = (unsigned long)secs * 1000UL;
  j.nextAt  = millis() + j.everyMs;
  j.runs    = 0;
  j.active  = true;
  s_jobs.push_back(j);

  io.out.printf("every: job %u added - \"%s\" every %lds\n",
                (unsigned)(s_jobs.size() - 1), line.c_str(), secs);
  return 0;
}

const Command CRON_CMDS[] = {
  {"every", cmd_every, "every <secs> <cmd> | --list | --del N", "run a command in the background", G_PROC},
};
const size_t CRON_CMDS_N = sizeof(CRON_CMDS) / sizeof(CRON_CMDS[0]);
