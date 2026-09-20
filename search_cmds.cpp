#include "shell.h"
#include <LittleFS.h>
#include <algorithm>

// ============================================================================
//  Search & scripting commands
//  Notes: grep is fixed-string (not regex). sed supports s/// only.
//         awk supports the {print ...} subset. These limits are in `help`.
// ============================================================================

static bool hasOpt(int argc, char **argv, char f) {
  for (int i = 1; i < argc; ++i)
    if (argv[i][0] == '-' && argv[i][1] != '-')
      for (const char *p = argv[i] + 1; *p; ++p) if (*p == f) return true;
  return false;
}

static String leaf(const String &name) {
  int sl = name.lastIndexOf('/');
  return (sl >= 0) ? name.substring(sl + 1) : name;
}

// grep/rg walk whole trees, so this runs once per candidate file: it has to be
// the bulk reader, not an append loop. Silent on failure by design - an
// unreadable file during a recursive search is skipped, not reported.
static bool readFile(const String &abs, String &out) {
  return readFileToString(abs, out, nullptr);
}

// Recursively collect file (not directory) paths under `root`.
static void collectFiles(const String &root, std::vector<String> &files) {
  File d = LittleFS.open(root);
  if (!d) return;
  File e = d.openNextFile();
  while (e) {
    String nm = leaf(String(e.name()));
    String child = root; if (!child.endsWith("/")) child += "/"; child = normalizePath(child + nm);
    bool dir = e.isDirectory();
    e.close();
    if (dir) collectFiles(child, files);
    else files.push_back(child);
    e = d.openNextFile();
  }
  d.close();
}

static bool lineMatch(const String &line, const String &pat, bool icase) {
  if (!icase) return line.indexOf(pat) >= 0;
  String a = line, b = pat; a.toLowerCase(); b.toLowerCase();
  return a.indexOf(b) >= 0;
}

// ---- grep ------------------------------------------------------------------
static int cmd_grep(int argc, char **argv, ShellIO &io) {
  bool icase = hasOpt(argc, argv, 'i'), num = hasOpt(argc, argv, 'n');
  bool inv = hasOpt(argc, argv, 'v'), cnt = hasOpt(argc, argv, 'c');
  String pat;
  std::vector<String> files;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-' && argv[i][1] != 0) continue;
    if (pat.length() == 0) pat = argv[i];
    else files.push_back(argv[i]);
  }
  if (pat.length() == 0) { io.out.println(F("usage: grep [-invc] PATTERN [file...]")); return 2; }

  bool multi = files.size() > 1;
  int total = 0, rc = 1;

  auto scan = [&](const String &data, const String &tag) {
    std::vector<String> lines; splitLines(data, lines);
    int fileCount = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
      bool m = lineMatch(lines[i], pat, icase);
      if (inv) m = !m;
      if (!m) continue;
      fileCount++; total++; rc = 0;
      if (cnt) continue;
      if (multi) { io.out.print(tag); io.out.print(':'); }
      if (num) { io.out.print((unsigned)(i + 1)); io.out.print(':'); }
      io.out.println(lines[i]);
    }
    if (cnt) { if (multi) { io.out.print(tag); io.out.print(':'); } io.out.println(fileCount); }
  };

  if (io.hasIn()) { scan(*io.in, ""); }
  else if (files.empty()) { return 1; }
  else {
    for (auto &fn : files) {
      String data;
      if (!readFile(resolvePath(fn), data)) { io.out.print(fn); io.out.println(F(": cannot read")); continue; }
      scan(data, fn);
    }
  }
  (void)total;
  return rc;
}

// ---- rg (recursive grep) ---------------------------------------------------
static int cmd_rg(int argc, char **argv, ShellIO &io) {
  bool icase = hasOpt(argc, argv, 'i');
  String pat, root;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-' && argv[i][1] != 0) continue;
    if (pat.length() == 0) pat = argv[i];
    else root = argv[i];
  }
  if (pat.length() == 0) { io.out.println(F("usage: rg [-i] PATTERN [path]")); return 2; }
  String base = root.length() ? resolvePath(root) : g_cwd;
  std::vector<String> files;
  if (isDir(base)) collectFiles(base, files);
  else files.push_back(base);
  int rc = 1;
  for (auto &fn : files) {
    String data;
    if (!readFile(fn, data)) continue;
    std::vector<String> lines; splitLines(data, lines);
    for (size_t i = 0; i < lines.size(); ++i)
      if (lineMatch(lines[i], pat, icase)) {
        io.out.print(fn); io.out.print(':'); io.out.print((unsigned)(i + 1)); io.out.print(':');
        io.out.println(lines[i]); rc = 0;
      }
  }
  return rc;
}

// ---- find ------------------------------------------------------------------
static void findWalk(const String &root, const String &namePat, char typ, ShellIO &io) {
  // print the root entry itself as '.'-style? print matching root then descend
  File d = LittleFS.open(root);
  if (!d) return;
  File e = d.openNextFile();
  while (e) {
    String nm = leaf(String(e.name()));
    String child = root; if (!child.endsWith("/")) child += "/"; child = normalizePath(child + nm);
    bool dir = e.isDirectory();
    e.close();
    bool okType = (typ == 0) || (typ == 'd' && dir) || (typ == 'f' && !dir);
    bool okName = (namePat.length() == 0) || matchWild(nm, namePat);
    if (okType && okName) io.out.println(child);
    if (dir) findWalk(child, namePat, typ, io);
    e = d.openNextFile();
  }
  d.close();
}

static int cmd_find(int argc, char **argv, ShellIO &io) {
  String root; String namePat; char typ = 0;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-name" && i + 1 < argc) namePat = argv[++i];
    else if (a == "-type" && i + 1 < argc) typ = argv[++i][0];
    else if (a[0] != '-' && root.length() == 0) root = a;
  }
  String base = root.length() ? resolvePath(root) : g_cwd;
  if (!isDir(base)) { io.out.println(base); return 0; }
  findWalk(base, namePat, typ, io);
  return 0;
}

// ---- locate ----------------------------------------------------------------
static int cmd_locate(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: locate name")); return 1; }
  String needle = argv[1]; needle.toLowerCase();
  std::vector<String> files;
  collectFiles("/", files);
  int rc = 1;
  for (auto &f : files) {
    String low = f; low.toLowerCase();
    if (low.indexOf(needle) >= 0) { io.out.println(f); rc = 0; }
  }
  return rc;
}

// ---- sed (s/// subset) -----------------------------------------------------
static int cmd_sed(int argc, char **argv, ShellIO &io) {
  String prog;
  std::vector<String> files;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-e" && i + 1 < argc) prog = argv[++i];
    else if (a[0] != '-' && prog.length() == 0) prog = a;
    else if (a[0] != '-') files.push_back(a);
  }
  if (prog.length() < 3 || prog[0] != 's') { io.out.println(F("sed: only s/pat/rep/[g] is supported")); return 1; }
  char d = prog[1];
  int p1 = 2, p2 = prog.indexOf(d, p1);
  if (p2 < 0) { io.out.println(F("sed: bad expression")); return 1; }
  int p3 = prog.indexOf(d, p2 + 1);
  if (p3 < 0) { io.out.println(F("sed: bad expression")); return 1; }
  String pat = prog.substring(p1, p2);
  String rep = prog.substring(p2 + 1, p3);
  String flags = prog.substring(p3 + 1);
  bool g = flags.indexOf('g') >= 0;
  if (pat.length() == 0) { io.out.println(F("sed: empty pattern")); return 1; }

  String data;
  if (io.hasIn()) data = *io.in;
  else for (auto &f : files) readFile(resolvePath(f), data);
  std::vector<String> lines; splitLines(data, lines);
  for (auto &line : lines) {
    String out; int pos = 0; bool done = false;
    while (pos <= (int)line.length()) {
      int at = line.indexOf(pat, pos);
      if (at < 0 || done) { out += line.substring(pos); break; }
      out += line.substring(pos, at);
      out += rep;
      pos = at + pat.length();
      if (!g) done = true;
    }
    io.out.println(out);
  }
  return 0;
}

// ---- awk ({print ...} subset) ----------------------------------------------
static void splitFields(const String &line, char fs, std::vector<String> &out) {
  out.clear();
  if (fs == 0) {  // whitespace, collapse
    String cur;
    for (size_t i = 0; i < line.length(); ++i) {
      char c = line[i];
      if (c == ' ' || c == '\t') { if (cur.length()) { out.push_back(cur); cur = ""; } }
      else cur += c;
    }
    if (cur.length()) out.push_back(cur);
  } else {
    String cur;
    for (size_t i = 0; i < line.length(); ++i) { if (line[i] == fs) { out.push_back(cur); cur = ""; } else cur += line[i]; }
    out.push_back(cur);
  }
}

static int cmd_awk(int argc, char **argv, ShellIO &io) {
  char fs = 0;
  String prog;
  std::vector<String> files;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a.startsWith("-F")) { String v = a.length() > 2 ? a.substring(2) : (i + 1 < argc ? String(argv[++i]) : String("")); if (v.length()) fs = v[0]; }
    else if (a[0] != '-' && prog.length() == 0) prog = a;
    else if (a[0] != '-') files.push_back(a);
  }
  // extract body between { }
  int b1 = prog.indexOf('{'), b2 = prog.lastIndexOf('}');
  String body = (b1 >= 0 && b2 > b1) ? prog.substring(b1 + 1, b2) : prog;
  body.trim();

  String data;
  if (io.hasIn()) data = *io.in;
  else for (auto &f : files) readFile(resolvePath(f), data);
  std::vector<String> lines; splitLines(data, lines);

  int pi = body.indexOf("print");
  for (auto &line : lines) {
    std::vector<String> f; splitFields(line, fs, f);
    if (pi < 0 || body.length() == 0) { io.out.println(line); continue; }  // default action
    String args = body.substring(pi + 5); args.trim();
    if (args.length() == 0) { io.out.println(line); continue; }            // bare print -> $0
    // split print args by comma
    String outLine; int start = 0;
    for (int i = 0; i <= (int)args.length(); ++i) {
      if (i == (int)args.length() || args[i] == ',') {
        String tok = args.substring(start, i); tok.trim();
        start = i + 1;
        if (tok.length() == 0) continue;
        if (outLine.length()) outLine += ' ';
        if (tok[0] == '$') {
          int idx = tok.substring(1).toInt();
          if (idx == 0) outLine += line;
          else if (idx >= 1 && idx <= (int)f.size()) outLine += f[idx - 1];
        } else if (tok[0] == '"' && tok.endsWith("\"")) {
          outLine += tok.substring(1, tok.length() - 1);
        } else {
          outLine += tok;
        }
      }
    }
    io.out.println(outLine);
  }
  return 0;
}

// ---- xargs -----------------------------------------------------------------
static int cmd_xargs(int argc, char **argv, ShellIO &io) {
  int perCall = 0;
  std::vector<String> base;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-n" && i + 1 < argc) perCall = atoi(argv[++i]);
    else base.push_back(a);
  }
  if (base.empty()) base.push_back("echo");
  if (!io.hasIn()) return 0;

  // tokenize stdin on whitespace
  std::vector<String> toks; String cur;
  const String &in = *io.in;
  for (size_t i = 0; i <= in.length(); ++i) {
    char c = (i < in.length()) ? in[i] : ' ';
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { if (cur.length()) { toks.push_back(cur); cur = ""; } }
    else cur += c;
  }

  auto runBatch = [&](int from, int to) {
    String line;
    for (auto &b : base) { line += b; line += ' '; }
    for (int k = from; k < to; ++k) { line += toks[k]; line += ' '; }
    runLine(line, io.out);
  };

  if (perCall <= 0) runBatch(0, toks.size());
  else for (int i = 0; i < (int)toks.size(); i += perCall) runBatch(i, std::min((int)toks.size(), i + perCall));
  return 0;
}

// ---- which / type / command / whereis --------------------------------------
static int cmd_which(int argc, char **argv, ShellIO &io) {
  int rc = 0;
  for (int i = 1; i < argc; ++i) {
    if (findCommand(argv[i])) { io.out.print(argv[i]); io.out.println(F(": shell built-in")); }
    else rc = 1;
  }
  return rc;
}
static int cmd_type(int argc, char **argv, ShellIO &io) {
  int rc = 0;
  for (int i = 1; i < argc; ++i) {
    if (findCommand(argv[i])) { io.out.print(argv[i]); io.out.println(F(" is a shell builtin")); }
    else { io.out.print(argv[i]); io.out.println(F(": not found")); rc = 1; }
  }
  return rc;
}
static int cmd_whereis(int argc, char **argv, ShellIO &io) {
  for (int i = 1; i < argc; ++i) {
    io.out.print(argv[i]); io.out.print(':');
    if (findCommand(argv[i])) io.out.println(F(" built-in"));
    else io.out.println();
  }
  return 0;
}
static int cmd_command(int argc, char **argv, ShellIO &io) {
  if (argc >= 3 && String(argv[1]) == "-v") {
    return findCommand(argv[2]) ? (io.out.println(argv[2]), 0) : 1;
  }
  if (argc < 2) return 0;
  const Command *c = findCommand(argv[1]);
  if (!c) { io.out.print(argv[1]); io.out.println(F(": not found")); return 127; }
  return c->fn(argc - 1, argv + 1, io);
}

const Command SEARCH_CMDS[] = {
  {"grep",    cmd_grep,    "grep [-invc] PAT [f]", "search lines (fixed string)",     G_SEARCH},
  {"rg",      cmd_rg,      "rg [-i] PAT [path]",   "recursive grep",                  G_SEARCH},
  {"find",    cmd_find,    "find [p] -name -type", "walk the tree, match entries",    G_SEARCH},
  {"locate",  cmd_locate,  "locate name",          "find files by name (whole FS)",   G_SEARCH},
  {"sed",     cmd_sed,     "sed s/pat/rep/[g]",    "stream edit (s/// only)",         G_SEARCH},
  {"awk",     cmd_awk,     "awk '{print $N}'",     "field printer subset",            G_SEARCH},
  {"xargs",   cmd_xargs,   "xargs [-n N] cmd",     "build commands from stdin",       G_SEARCH},
  {"which",   cmd_which,   "which name...",        "locate a command",                G_SEARCH},
  {"whereis", cmd_whereis, "whereis name...",      "locate a command",                G_SEARCH},
  {"type",    cmd_type,    "type name...",         "describe a command",              G_SEARCH},
  {"command", cmd_command, "command [-v] name",    "run/lookup a command",            G_SEARCH},
};
const size_t SEARCH_CMDS_N = sizeof(SEARCH_CMDS) / sizeof(SEARCH_CMDS[0]);
