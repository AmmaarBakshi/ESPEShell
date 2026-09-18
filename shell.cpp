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

    String redir;
    bool append = false;
    bool hasRedir = false;
    if (last) hasRedir = parseRedirect(args, redir, append);

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
        f.print(sbuf.s);
        f.close();
      }
    } else {
      stageIn = sbuf.s;
      haveIn = true;
    }
  }
  return rc;
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
  io.out.println(F("Pipes '|' and redirection '>' '>>' are supported."));
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
};
const size_t CMD_TABLE_COUNT = sizeof(CMD_TABLES) / sizeof(CMD_TABLES[0]);
