#include "shell.h"
#include <ctype.h>

// ============================================================================
//  Scripting helpers: commands that drive other commands, plus the small
//  shell utilities scripts lean on (seq, yes, true/false, test, expr).
// ============================================================================

// Joins argv[from..argc) back into a single command line for re-dispatch.
static String joinArgs(int argc, char **argv, int from) {
  String line;
  for (int i = from; i < argc; ++i) {
    if (line.length()) line += ' ';
    line += argv[i];
  }
  return line;
}

// ---- watch : re-run a command every N seconds until Ctrl-C -----------------
static int cmd_watch(int argc, char **argv, ShellIO &io) {
  int every = 2;      // seconds, like watch(1)
  int i = 1;
  bool clear = true;
  for (; i < argc; ++i) {
    String a = argv[i];
    if (a == "-n" && i + 1 < argc) { every = String(argv[++i]).toInt(); }
    else if (a.startsWith("-n") && a.length() > 2) { every = a.substring(2).toInt(); }
    else if (a == "-t" || a == "--no-clear") { clear = false; }
    else break;
  }
  if (i >= argc) {
    io.out.println(F("usage: watch [-n secs] [-t] <command...>   (Ctrl-C to stop)"));
    return 1;
  }
  if (every < 1) every = 1;
  String line = joinArgs(argc, argv, i);

  unsigned long round = 0;
  for (;;) {
    if (clear) io.out.print(F("\033[2J\033[H"));
    io.out.printf("Every %ds: %s   (#%lu, Ctrl-C to stop)\n\n", every, line.c_str(), ++round);
    runLine(line, io.out, io.rawIn);
    if (shellWait(io, every * 1000)) { io.out.println(F("\nwatch: stopped.")); return 0; }
  }
}

const Command SCRIPT_CMDS[] = {
  {"watch", cmd_watch, "watch [-n secs] [-t] cmd", "re-run a command periodically", G_SEARCH},
};
const size_t SCRIPT_CMDS_N = sizeof(SCRIPT_CMDS) / sizeof(SCRIPT_CMDS[0]);
