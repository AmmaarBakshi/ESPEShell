#include "shell.h"
#include "config.h"
#include <LittleFS.h>
#include <ctype.h>

// ============================================================================
//  Shell state
// ============================================================================
String g_cwd = "/";
String g_hostname = ESPE_HOSTNAME;
String g_user = ESPE_USER;
String g_telnetPeer = "";
volatile uint32_t g_bytesIn = 0;
volatile uint32_t g_bytesOut = 0;

static std::vector<std::pair<String, String>> s_env;
static std::vector<std::pair<String, String>> s_alias;

// ============================================================================
//  Environment variables
// ============================================================================
String envGet(const String &key) {
  if (key == "PWD") return g_cwd;
  for (auto &kv : s_env)
    if (kv.first == key) return kv.second;
  if (key == "USER" || key == "LOGNAME") return g_user;
  if (key == "HOSTNAME") return g_hostname;
  if (key == "HOME") return "/";
  if (key == "SHELL") return "/espeshell";
  return "";
}

void envSet(const String &key, const String &val) {
  for (auto &kv : s_env)
    if (kv.first == key) { kv.second = val; return; }
  s_env.push_back(std::make_pair(key, val));
}

bool envUnset(const String &key) {
  for (size_t i = 0; i < s_env.size(); ++i)
    if (s_env[i].first == key) { s_env.erase(s_env.begin() + i); return true; }
  return false;
}

const std::vector<std::pair<String, String>> &envAll() { return s_env; }

// ============================================================================
//  Aliases (RAM only - put your favourites in /boot.sh to get them at boot)
// ============================================================================
String aliasGet(const String &name) {
  for (auto &kv : s_alias)
    if (kv.first == name) return kv.second;
  return "";
}

void aliasSet(const String &name, const String &val) {
  for (auto &kv : s_alias)
    if (kv.first == name) { kv.second = val; return; }
  s_alias.push_back(std::make_pair(name, val));
}

bool aliasUnset(const String &name) {
  for (size_t i = 0; i < s_alias.size(); ++i)
    if (s_alias[i].first == name) { s_alias.erase(s_alias.begin() + i); return true; }
  return false;
}

const std::vector<std::pair<String, String>> &aliasAll() { return s_alias; }

// ============================================================================
//  Path helpers
// ============================================================================
String normalizePath(const String &path) {
  std::vector<String> parts;
  String cur;
  for (size_t i = 0; i <= path.length(); ++i) {
    char c = (i < path.length()) ? path[i] : '/';
    if (c == '/') {
      if (cur.length()) {
        if (cur == ".") {
        } else if (cur == "..") {
          if (!parts.empty()) parts.pop_back();
        } else {
          parts.push_back(cur);
        }
      }
      cur = "";
    } else {
      cur += c;
    }
  }
  if (parts.empty()) return "/";
  String r;
  for (size_t i = 0; i < parts.size(); ++i) { r += "/"; r += parts[i]; }
  return r;
}

String resolvePath(const String &p) {
  String full;
  if (p.length() && p[0] == '/') full = p;
  else {
    full = g_cwd;
    if (!full.endsWith("/")) full += "/";
    full += p;
  }
  return normalizePath(full);
}

String baseName(const String &p) {
  String n = normalizePath(p);
  if (n == "/") return "/";
  int slash = n.lastIndexOf('/');
  return n.substring(slash + 1);
}

String dirName(const String &p) {
  String n = normalizePath(p);
  if (n == "/") return "/";
  int slash = n.lastIndexOf('/');
  if (slash <= 0) return "/";
  return n.substring(0, slash);
}

bool pathExists(const String &abs) {
  if (abs == "/") return true;
  return LittleFS.exists(abs);
}

bool isDir(const String &abs) {
  if (abs == "/") return true;
  File f = LittleFS.open(abs, "r");
  if (!f) return false;
  bool d = f.isDirectory();
  f.close();
  return d;
}

// ============================================================================
//  Command history
// ============================================================================
#define HISTORY_MAX 30
static std::vector<String> s_history;   // s_history.back() is the most recent

void historyAdd(const String &line) {
  String t = line; t.trim();
  if (t.length() == 0) return;
  if (!s_history.empty() && s_history.back() == t) return;  // skip immediate repeats
  s_history.push_back(t);
  if (s_history.size() > HISTORY_MAX) s_history.erase(s_history.begin());
}

size_t historyCount() { return s_history.size(); }

void historyClear() { s_history.clear(); }

String historyGet(int indexFromEnd) {
  int i = (int)s_history.size() - 1 - indexFromEnd;
  if (i < 0 || i >= (int)s_history.size()) return "";
  return s_history[i];
}

// ============================================================================
//  Tab completion
// ============================================================================
String completeLine(const String &partial, Print &out) {
  int sp = partial.length();
  while (sp > 0 && partial[sp - 1] != ' ') sp--;
  String head = partial.substring(0, sp);   // text before the token (kept as-is)
  String tok = partial.substring(sp);        // the token being completed
  bool firstToken = (head.length() == 0);

  std::vector<String> matches;
  String commonPrefix;

  auto consider = [&](const String &full) {
    if (!full.startsWith(tok)) return;
    matches.push_back(full);
    if (matches.size() == 1) { commonPrefix = full; return; }
    size_t k = 0;
    while (k < commonPrefix.length() && k < full.length() && commonPrefix[k] == full[k]) k++;
    commonPrefix = commonPrefix.substring(0, k);
  };

  if (firstToken) {
    for (size_t t = 0; t < CMD_TABLE_COUNT; ++t) {
      const CmdTable &tab = CMD_TABLES[t];
      for (size_t i = 0; i < tab.count; ++i) consider(String(tab.cmds[i].name));
    }
  } else {
    String dirPart, leafPrefix;
    int slash = tok.lastIndexOf('/');
    if (slash < 0) { leafPrefix = tok; }
    else { dirPart = tok.substring(0, slash + 1); leafPrefix = tok.substring(slash + 1); }
    String absDir = dirPart.length() ? resolvePath(dirPart) : g_cwd;
    if (isDir(absDir)) {
      File d = LittleFS.open(absDir);
      if (d) {
        File e = d.openNextFile();
        while (e) {
          String nm = String(e.name());
          int sl = nm.lastIndexOf('/');
          if (sl >= 0) nm = nm.substring(sl + 1);
          bool dir = e.isDirectory();
          e.close();
          if (nm.startsWith(leafPrefix)) consider(dirPart + nm + (dir ? "/" : ""));
          e = d.openNextFile();
        }
        d.close();
      }
    }
  }

  if (matches.empty()) return partial;
  if (matches.size() == 1) return head + matches[0] + (firstToken ? " " : "");
  if (commonPrefix.length() > tok.length()) return head + commonPrefix;

  out.println();
  String row;
  for (size_t i = 0; i < matches.size(); ++i) {
    row += matches[i]; row += "  ";
    if (row.length() > 60) { out.println(row); row = ""; }
  }
  if (row.length()) out.println(row);
  return partial;
}

// ============================================================================
//  Shared command helpers
// ============================================================================
bool collectInput(int argc, char **argv, int firstFileArg, ShellIO &io, String &out) {
  out = "";
  if (io.hasIn()) { out = *io.in; return true; }
  bool any = false;
  for (int i = firstFileArg; i < argc; ++i) {
    if (argv[i][0] == '-') continue;  // skip option-looking args
    String ap = resolvePath(argv[i]);
    File f = LittleFS.open(ap, "r");
    if (!f || f.isDirectory()) {
      io.out.print(argv[i]);
      io.out.println(": cannot read");
      if (f) f.close();
      continue;
    }
    uint8_t buf[128];
    while (true) {
      int n = f.read(buf, sizeof(buf));
      if (n <= 0) break;
      for (int k = 0; k < n; ++k) out += (char)buf[k];
    }
    f.close();
    any = true;
  }
  return any;
}

void splitLines(const String &s, std::vector<String> &lines) {
  lines.clear();
  String cur;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '\n') { lines.push_back(cur); cur = ""; }
    else if (c == '\r') { /* strip */ }
    else cur += c;
  }
  if (cur.length()) lines.push_back(cur);
}

bool matchWild(const String &t, const String &p) {
  int ti = 0, pi = 0, star = -1, mark = 0;
  int tn = t.length(), pn = p.length();
  while (ti < tn) {
    if (pi < pn && (p[pi] == t[ti] || p[pi] == '?')) { ti++; pi++; }
    else if (pi < pn && p[pi] == '*') { star = pi++; mark = ti; }
    else if (star != -1) { pi = star + 1; ti = ++mark; }
    else return false;
  }
  while (pi < pn && p[pi] == '*') pi++;
  return pi == pn;
}

String humanBytes(uint64_t n) {
  const char *u[] = {"B", "K", "M", "G", "T"};
  double v = (double)n;
  int i = 0;
  while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
  char b[24];
  if (i == 0) snprintf(b, sizeof(b), "%u%s", (unsigned)n, u[0]);
  else snprintf(b, sizeof(b), "%.1f%s", v, u[i]);
  return String(b);
}

// Sleeps for `ms`, but returns early with true if Ctrl-C (0x03) arrives on the
// live connection. Long-running commands (blink, watch, ping loops) call this
// instead of delay() so they never hold the session hostage.
bool shellWait(ShellIO &io, int ms) {
  unsigned long start = millis();
  for (;;) {
    if (io.rawIn && io.rawIn->available()) {
      int c = io.rawIn->read();
      g_bytesIn++;
      if (c == 0x03) return true;
    }
    if (millis() - start >= (unsigned long)ms) return false;
    delay(2);
  }
}

String expandVars(const String &s) {
  String r;
  for (size_t i = 0; i < s.length();) {
    char c = s[i];
    if (c == '$' && i + 1 < s.length()) {
      size_t j = i + 1;
      bool brace = false;
      if (s[j] == '{') { brace = true; j++; }
      String name;
      while (j < s.length() && (isalnum((int)s[j]) || s[j] == '_')) { name += s[j]; j++; }
      if (brace && j < s.length() && s[j] == '}') j++;
      if (name.length()) { r += envGet(name); i = j; }
      else { r += c; i++; }
    } else {
      r += c; i++;
    }
  }
  return r;
}

// ============================================================================
//  Output capture (for pipes / redirection)
// ============================================================================
class StringPrint : public Print {
 public:
  String s;
  size_t write(uint8_t c) override { s += (char)c; return 1; }
  size_t write(const uint8_t *b, size_t n) override {
    s.reserve(s.length() + n);
    for (size_t i = 0; i < n; ++i) s += (char)b[i];
    return n;
  }
};

// ============================================================================
//  Command lookup
// ============================================================================
const Command *findCommand(const char *name) {
  for (size_t t = 0; t < CMD_TABLE_COUNT; ++t) {
    const CmdTable &tab = CMD_TABLES[t];
    for (size_t i = 0; i < tab.count; ++i)
      if (strcmp(tab.cmds[i].name, name) == 0) return &tab.cmds[i];
  }
  return nullptr;
}

const char *groupName(uint8_t g) {
  switch (g) {
    case G_FS:      return "Filesystem";
    case G_TEXT:    return "Text processing";
    case G_SEARCH:  return "Search & scripting";
    case G_SYS:     return "System & info";
    case G_PROC:    return "Processes / jobs";
    case G_NET:     return "Networking";
    case G_ENV:     return "Environment";
    case G_ARCHIVE: return "Archive / packages";
    case G_ESP:     return "ESP32 specific";
    case G_CORE:    return "Shell built-ins";
    case G_XFER:    return "File transfer";
    default:        return "Other";
  }
}

// ============================================================================
//  Tokenizer, pipe split, redirection
// ============================================================================
static std::vector<String> splitPipes(const String &line) {
  std::vector<String> segs;
  String cur;
  char q = 0;
  for (size_t i = 0; i < line.length(); ++i) {
    char c = line[i];
    if (q) { cur += c; if (c == q) q = 0; }
    else if (c == '\'' || c == '"') { q = c; cur += c; }
    else if (c == '|') { segs.push_back(cur); cur = ""; }
    else cur += c;
  }
  segs.push_back(cur);
  return segs;
}

static int tokenize(const String &seg, std::vector<String> &args) {
  args.clear();
  String cur;
  bool has = false;
  char q = 0;
  for (size_t i = 0; i < seg.length(); ++i) {
    char c = seg[i];
    if (q) {
      if (c == q) { q = 0; }
      else if (c == '\\' && q == '"' && i + 1 < seg.length()) { cur += seg[++i]; }
      else cur += c;
      has = true;
    } else if (c == '\'' || c == '"') {
      q = c; has = true;
    } else if (c == '\\' && i + 1 < seg.length()) {
      cur += seg[++i]; has = true;
    } else if (c == ' ' || c == '\t') {
      if (has) { args.push_back(cur); cur = ""; has = false; }
    } else {
      cur += c; has = true;
    }
  }
  if (has) args.push_back(cur);
  return args.size();
}

// Pull a trailing >/>> redirection out of the arg list. Returns true if found.
static bool parseRedirect(std::vector<String> &args, String &redir, bool &append) {
  redir = "";
  append = false;
  for (size_t i = 0; i < args.size(); ++i) {
    String &t = args[i];
    if (t == ">" || t == ">>") {
      append = (t == ">>");
      if (i + 1 < args.size()) {
        redir = args[i + 1];
        args.erase(args.begin() + i, args.begin() + i + 2);
      } else {
        args.erase(args.begin() + i);
      }
      return true;
    } else if (t.startsWith(">>")) {
      append = true; redir = t.substring(2); args.erase(args.begin() + i); return true;
    } else if (t.startsWith(">")) {
      append = false; redir = t.substring(1); args.erase(args.begin() + i); return true;
    }
  }
  return false;
}

// Pull a '<' input redirection out of the arg list. Returns true if found.
static bool parseInRedirect(std::vector<String> &args, String &inFile) {
  inFile = "";
  for (size_t i = 0; i < args.size(); ++i) {
    String &t = args[i];
    if (t == "<") {
      if (i + 1 < args.size()) {
        inFile = args[i + 1];
        args.erase(args.begin() + i, args.begin() + i + 2);
      } else {
        args.erase(args.begin() + i);
      }
      return true;
    } else if (t.startsWith("<")) {
      inFile = t.substring(1);
      args.erase(args.begin() + i);
      return true;
    }
  }
  return false;
}

// Commands write terminal line endings (\r\n, which Telnet/serial terminals
// need). Files should hold plain \n, so strip the CR on the way to disk.
String toUnixEol(const String &s) {
  String o;
  o.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i)
    if (s[i] != '\r') o += s[i];
  return o;
}

// Replaces a leading alias with its expansion, repeatedly (so one alias may
// build on another). A word is expanded at most once per line, which is what
// stops `alias ls='ls -l'` from recursing forever.
static void expandAliases(std::vector<String> &args) {
  std::vector<String> used;
  for (int depth = 0; depth < 8 && !args.empty(); ++depth) {
    String head = args[0];
    bool seen = false;
    for (auto &u : used) if (u == head) { seen = true; break; }
    if (seen) return;
    String val = aliasGet(head);
    if (val.length() == 0) return;
    used.push_back(head);
    std::vector<String> expanded;
    tokenize(val, expanded);
    if (expanded.empty()) return;
    args.erase(args.begin());
    args.insert(args.begin(), expanded.begin(), expanded.end());
  }
}

// ============================================================================
//  Dispatch: pipes + redirection
// ============================================================================
int runLine(const String &lineIn, Print &realOut, Stream *rawIn) {
  String line = lineIn;
  line.trim();
  if (line.length() == 0) return 0;
  if (line[0] == '#') return 0;

  std::vector<String> segs = splitPipes(line);
  String stageIn;
  bool haveIn = false;
  int rc = 0;

  for (size_t s = 0; s < segs.size(); ++s) {
    bool last = (s + 1 == segs.size());

    std::vector<String> args;
    tokenize(segs[s], args);
    expandAliases(args);

    String redir;
    bool append = false;
    bool hasRedir = false;
    if (last) hasRedir = parseRedirect(args, redir, append);

    // '<' feeds a file in as the first stage's stdin.
    if (s == 0) {
      String inFile;
      if (parseInRedirect(args, inFile)) {
        String abs = resolvePath(inFile);
        File f = LittleFS.open(abs, "r");
        if (!f || f.isDirectory()) {
          realOut.print(inFile);
          realOut.println(": cannot read");
          if (f) f.close();
          return 1;
        }
        stageIn = "";
        uint8_t buf[128];
        while (true) {
          int n = f.read(buf, sizeof(buf));
          if (n <= 0) break;
          for (int k = 0; k < n; ++k) stageIn += (char)buf[k];
        }
        f.close();
        haveIn = true;
      }
    }

    if (args.empty()) {
      if (last) return rc;
      haveIn = false; stageIn = "";
      continue;
    }

    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &a : args) argv.push_back((char *)a.c_str());

    const Command *c = findCommand(argv[0]);
    if (!c) {
      realOut.print(argv[0]);
      realOut.println(": command not found");
      return 127;
    }

    StringPrint sbuf;
    Print *outp;
    if (last && !hasRedir) outp = &realOut;
    else outp = &sbuf;  // capture (piped stage, or redirected last stage)

    ShellIO io(*outp, haveIn ? &stageIn : nullptr, rawIn);
    rc = c->fn((int)argv.size(), argv.data(), io);

    if (last) {
      if (hasRedir) {
        String rp = resolvePath(redir);
        File f = LittleFS.open(rp, append ? "a" : "w");
        if (!f) {
          realOut.print(redir);
          realOut.println(": cannot open for writing");
          return 1;
        }
        f.print(toUnixEol(sbuf.s));
        f.close();
      }
    } else {
      stageIn = sbuf.s;
      haveIn = true;
    }
  }
  return rc;
}

// Runs a line with its output captured instead of printed. Used when the same
// output has to reach more than one place (see the `every` jobs drained in
// ESPEShell.ino, which print to Serial and to the live Telnet session).
String runCapture(const String &line) {
  StringPrint buf;
  runLine(line, buf);
  return buf.s;
}

// ============================================================================
//  Prompt & banner
// ============================================================================
void printPrompt(Print &out) {
  out.print(g_user);
  out.print('@');
  out.print(g_hostname);
  out.print(':');
  out.print(g_cwd);
  out.print(g_user == "root" ? "# " : "$ ");
}

void printBanner(Print &out) {
  out.println();
  out.println(F("  ______ _____ _____  ______ _____ _          _ _ "));
  out.println(F(" |  ____/ ____|  __ \\|  ____/ ____| |        | | |"));
  out.println(F(" | |__ | (___ | |__) | |__ | (___ | |__   ___| | |"));
  out.println(F(" |  __| \\___ \\|  ___/|  __| \\___ \\| '_ \\ / _ \\ | |"));
  out.println(F(" | |____ ___) | |    | |____ ___) | | | |  __/ | |"));
  out.println(F(" |______|____/|_|    |______|____/|_| |_|\\___|_|_|"));
  out.print(F("  ESPEShell v"));
  out.print(F(ESPE_VERSION));
  out.println(F("   -   type 'help' for commands"));
  out.println();
}

// ============================================================================
//  CORE built-in commands
// ============================================================================
static void helpList(ShellIO &io, int onlyGroup /* -1 = all */) {
  for (uint8_t g = 0; g < G_COUNT; ++g) {
    if (onlyGroup >= 0 && g != (uint8_t)onlyGroup) continue;
    bool header = false;
    for (size_t t = 0; t < CMD_TABLE_COUNT; ++t) {
      const CmdTable &tab = CMD_TABLES[t];
      for (size_t i = 0; i < tab.count; ++i) {
        if (tab.cmds[i].group != g) continue;
        if (!header) {
          io.out.print(F("\n"));
          io.out.print(groupName(g));
          io.out.println(F(":"));
          header = true;
        }
        char line[80];
        snprintf(line, sizeof(line), "  %-12s %s", tab.cmds[i].name, tab.cmds[i].summary);
        io.out.println(line);
      }
    }
  }
}

static int cmd_help(int argc, char **argv, ShellIO &io) {
  if (argc >= 2 && strcmp(argv[1], "--esp") == 0) {
    io.out.println(F("ESP32-specific commands:"));
    helpList(io, G_ESP);
    return 0;
  }
  if (argc >= 2) {
    const Command *c = findCommand(argv[1]);
    if (!c) { io.out.print(argv[1]); io.out.println(F(": no such command")); return 1; }
    io.out.print(F("usage: ")); io.out.println(c->usage);
    io.out.print(F("   ")); io.out.println(c->summary);
    return 0;
  }
  io.out.println(F("ESPEShell - available commands (help <cmd> for usage):"));
  helpList(io, -1);
  io.out.println();
  io.out.println(F("Pipes '|' and redirection '<' '>' '>>' are supported."));
  return 0;
}

static int cmd_man(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("What manual page do you want?")); return 1; }
  const Command *c = findCommand(argv[1]);
  if (!c) { io.out.print(F("No manual entry for ")); io.out.println(argv[1]); return 1; }
  io.out.println(F("NAME"));
  io.out.print(F("    ")); io.out.print(c->name); io.out.print(F(" - ")); io.out.println(c->summary);
  io.out.println();
  io.out.println(F("SYNOPSIS"));
  io.out.print(F("    ")); io.out.println(c->usage);
  return 0;
}

static int cmd_whatis(int argc, char **argv, ShellIO &io) {
  if (argc < 2) return 1;
  int rc = 0;
  for (int a = 1; a < argc; ++a) {
    const Command *c = findCommand(argv[a]);
    if (c) { io.out.print(c->name); io.out.print(F(" - ")); io.out.println(c->summary); }
    else { io.out.print(argv[a]); io.out.println(F(": nothing appropriate.")); rc = 1; }
  }
  return rc;
}

static int cmd_apropos(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("apropos: what?")); return 1; }
  String kw = argv[1];
  kw.toLowerCase();
  for (size_t t = 0; t < CMD_TABLE_COUNT; ++t) {
    const CmdTable &tab = CMD_TABLES[t];
    for (size_t i = 0; i < tab.count; ++i) {
      String hay = String(tab.cmds[i].name) + " " + tab.cmds[i].summary;
      hay.toLowerCase();
      if (hay.indexOf(kw) >= 0) {
        io.out.print(tab.cmds[i].name);
        io.out.print(F(" - "));
        io.out.println(tab.cmds[i].summary);
      }
    }
  }
  return 0;
}

// ---- history : the interactive line history (shared by Serial + Telnet) ----
static int cmd_history(int argc, char **argv, ShellIO &io) {
  if (argc >= 2 && (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "--clear") == 0)) {
    historyClear();
    io.out.println(F("history cleared"));
    return 0;
  }
  size_t n = historyCount();
  for (size_t i = 0; i < n; ++i) {
    // historyGet(0) is the newest, so walk backwards to print oldest first.
    io.out.printf("%4u  %s\n", (unsigned)(i + 1), historyGet((int)(n - 1 - i)).c_str());
  }
  return 0;
}

// ---- alias / unalias -------------------------------------------------------
static int cmd_alias(int argc, char **argv, ShellIO &io) {
  if (argc < 2) {
    for (auto &kv : aliasAll()) {
      io.out.print(F("alias "));
      io.out.print(kv.first);
      io.out.print(F("='"));
      io.out.print(kv.second);
      io.out.println(F("'"));
    }
    return 0;
  }
  int rc = 0;
  for (int a = 1; a < argc; ++a) {
    String spec = argv[a];
    int eq = spec.indexOf('=');
    if (eq < 0) {                       // `alias name` -> show just that one
      String v = aliasGet(spec);
      if (v.length() == 0) { io.out.print(F("alias: ")); io.out.print(spec); io.out.println(F(": not found")); rc = 1; }
      else { io.out.print(F("alias ")); io.out.print(spec); io.out.print(F("='")); io.out.print(v); io.out.println(F("'")); }
      continue;
    }
    String name = spec.substring(0, eq);
    String val  = spec.substring(eq + 1);
    name.trim();
    if (name.length() == 0) { io.out.println(F("alias: empty name")); rc = 1; continue; }
    aliasSet(name, val);
  }
  return rc;
}

static int cmd_unalias(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: unalias name...")); return 1; }
  int rc = 0;
  for (int a = 1; a < argc; ++a)
    if (!aliasUnset(argv[a])) { io.out.print(argv[a]); io.out.println(F(": not an alias")); rc = 1; }
  return rc;
}

static int cmd_clear(int argc, char **argv, ShellIO &io) {
  io.out.print(F("\033[2J\033[H"));
  return 0;
}

static int cmd_exit(int argc, char **argv, ShellIO &io) {
  // The session layer (ESPEShell.ino) intercepts exit/logout on Telnet and
  // closes the socket. On Serial there is nothing to close.
  io.out.println(F("logout"));
  return 0;
}

// ---- sh : run stored shell commands from a LittleFS file -------------------
// Also used to auto-run /boot.sh at startup (see ESPEShell.ino's setup()).
static int cmd_sh(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: sh <file>   (runs one shell command per line)")); return 1; }
  String abs = resolvePath(argv[1]);
  File f = LittleFS.open(abs, "r");
  if (!f || f.isDirectory()) {
    io.out.print(argv[1]); io.out.println(F(": cannot read"));
    if (f) f.close();
    return 1;
  }
  String content;
  uint8_t buf[128];
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int k = 0; k < n; ++k) content += (char)buf[k];
  }
  f.close();

  std::vector<String> lines;
  splitLines(content, lines);
  int rc = 0;
  for (auto &l : lines) {
    String t = l; t.trim();
    if (t.length() == 0 || t[0] == '#') continue;  // blank lines / comments
    rc = runLine(l, io.out);
  }
  return rc;
}

const Command CORE_CMDS[] = {
  {"help",    cmd_help,    "help [--esp] [cmd]",  "list commands or show usage for one", G_CORE},
  {"man",     cmd_man,     "man <cmd>",           "show the manual entry for a command", G_CORE},
  {"whatis",  cmd_whatis,  "whatis <cmd>...",     "one-line description of a command",   G_CORE},
  {"apropos", cmd_apropos, "apropos <keyword>",   "search commands by keyword",          G_CORE},
  {"alias",   cmd_alias,   "alias [name=cmd]",    "define or list command aliases",      G_CORE},
  {"unalias", cmd_unalias, "unalias name...",     "remove a command alias",              G_CORE},
  {"history", cmd_history, "history [-c]",        "list (or clear) the command history",  G_CORE},
  {"clear",   cmd_clear,   "clear",               "clear the screen",                    G_CORE},
  {"sh",      cmd_sh,      "sh <file>",           "run stored shell commands from a file",G_CORE},
  {"exit",    cmd_exit,    "exit",                "end this session (Telnet)",           G_CORE},
  {"logout",  cmd_exit,    "logout",              "end this session (Telnet)",           G_CORE},
};
const size_t CORE_CMDS_N = sizeof(CORE_CMDS) / sizeof(CORE_CMDS[0]);

// ============================================================================
//  Aggregate command tables (extended as modules are added)
// ============================================================================
const CmdTable CMD_TABLES[] = {
  {CORE_CMDS, CORE_CMDS_N},
  {FS_CMDS, FS_CMDS_N},
  {TEXT_CMDS, TEXT_CMDS_N},
  {SEARCH_CMDS, SEARCH_CMDS_N},
  {SYS_CMDS, SYS_CMDS_N},
  {NET_CMDS, NET_CMDS_N},
  {MISC_CMDS, MISC_CMDS_N},
  {ESP_CMDS, ESP_CMDS_N},
  {XFER_CMDS, XFER_CMDS_N},
  {MQTT_CMDS, MQTT_CMDS_N},
  {SCRIPT_CMDS, SCRIPT_CMDS_N},
  {TIME_CMDS, TIME_CMDS_N},
  {HASH_CMDS, HASH_CMDS_N},
  {GPIO_CMDS, GPIO_CMDS_N},
  {DIAG_CMDS, DIAG_CMDS_N},
  {HTTPD_CMDS, HTTPD_CMDS_N},
};
const size_t CMD_TABLE_COUNT = sizeof(CMD_TABLES) / sizeof(CMD_TABLES[0]);
