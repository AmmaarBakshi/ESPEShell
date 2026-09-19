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

// ---- repeat : run a command N times ----------------------------------------
static int cmd_repeat(int argc, char **argv, ShellIO &io) {
  if (argc < 3) {
    io.out.println(F("usage: repeat <count> [-d ms] <command...>"));
    return 1;
  }
  long count = String(argv[1]).toInt();
  if (count <= 0) { io.out.println(F("repeat: count must be > 0")); return 1; }
  int i = 2, delayMs = 0;
  if (String(argv[i]) == "-d" && i + 1 < argc) { delayMs = String(argv[i + 1]).toInt(); i += 2; }
  if (i >= argc) { io.out.println(F("repeat: no command given")); return 1; }
  String line = joinArgs(argc, argv, i);

  int rc = 0;
  for (long n = 0; n < count; ++n) {
    rc = runLine(line, io.out, io.rawIn);
    if (delayMs > 0 && n + 1 < count && shellWait(io, delayMs)) {
      io.out.println(F("repeat: stopped."));
      return rc;
    }
  }
  return rc;
}

// ---- seq : print a number sequence -----------------------------------------
static int cmd_seq(int argc, char **argv, ShellIO &io) {
  long first = 1, incr = 1, last = 0;
  if (argc == 2)      { last = String(argv[1]).toInt(); }
  else if (argc == 3) { first = String(argv[1]).toInt(); last = String(argv[2]).toInt(); }
  else if (argc == 4) { first = String(argv[1]).toInt(); incr = String(argv[2]).toInt(); last = String(argv[3]).toInt(); }
  else { io.out.println(F("usage: seq [first [incr]] last")); return 1; }
  if (incr == 0) { io.out.println(F("seq: increment must not be 0")); return 1; }

  long guard = 0;   // a typo shouldn't be able to spam the session forever
  for (long v = first; (incr > 0) ? (v <= last) : (v >= last); v += incr) {
    io.out.println(v);
    if (++guard > 10000) { io.out.println(F("seq: stopped at 10000 lines")); return 1; }
  }
  return 0;
}

// ---- yes : repeat a string (bounded - this is an MCU, not a workstation) ---
static int cmd_yes(int argc, char **argv, ShellIO &io) {
  String word = (argc >= 2) ? joinArgs(argc, argv, 1) : String("y");
  long n = 100;   // bounded default; `yes -n N` for more
  int start = 1;
  if (argc >= 3 && String(argv[1]) == "-n") { n = String(argv[2]).toInt(); start = 3; word = (start < argc) ? joinArgs(argc, argv, start) : String("y"); }
  if (n <= 0 || n > 10000) n = 100;
  for (long i = 0; i < n; ++i) io.out.println(word);
  return 0;
}

// ---- true / false : exit-status primitives ---------------------------------
static int cmd_true(int argc, char **argv, ShellIO &io)  { return 0; }
static int cmd_false(int argc, char **argv, ShellIO &io) { return 1; }

const Command SCRIPT_CMDS[] = {
  {"seq",    cmd_seq,    "seq [first [incr]] last",  "print a number sequence",       G_SEARCH},
  {"yes",    cmd_yes,    "yes [-n N] [string]",      "repeat a string N times",       G_SEARCH},
  {"true",   cmd_true,   "true",                     "do nothing, succeed",           G_SEARCH},
  {"false",  cmd_false,  "false",                    "do nothing, fail",              G_SEARCH},
  {"repeat", cmd_repeat, "repeat N [-d ms] cmd",     "run a command N times",         G_SEARCH},
  {"watch", cmd_watch, "watch [-n secs] [-t] cmd", "re-run a command periodically", G_SEARCH},
};
const size_t SCRIPT_CMDS_N = sizeof(SCRIPT_CMDS) / sizeof(SCRIPT_CMDS[0]);
