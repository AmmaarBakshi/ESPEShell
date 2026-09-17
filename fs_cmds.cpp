#include "shell.h"
#include <LittleFS.h>
#include <algorithm>

// ============================================================================
//  Filesystem commands (LittleFS)
// ============================================================================

// ---- small helpers ---------------------------------------------------------
static bool hasFlag(int argc, char **argv, char f) {
  for (int i = 1; i < argc; ++i)
    if (argv[i][0] == '-' && argv[i][1] != '-')
      for (const char *p = argv[i] + 1; *p; ++p)
        if (*p == f) return true;
  return false;
}

// Return the n-th non-flag argument (0-based), or "" if none.
static String argAt(int argc, char **argv, int n) {
  int k = 0;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-' && argv[i][1] != 0) continue;
    if (k == n) return String(argv[i]);
    k++;
  }
  return String("");
}
static int nonFlagCount(int argc, char **argv) {
  int k = 0;
  for (int i = 1; i < argc; ++i)
    if (!(argv[i][0] == '-' && argv[i][1] != 0)) k++;
  return k;
}

static String leaf(const String &name) {
  int sl = name.lastIndexOf('/');
  return (sl >= 0) ? name.substring(sl + 1) : name;
}
static String childPath(const String &dir, File &e) {
  String nm = leaf(String(e.name()));
  String d = dir;
  if (!d.endsWith("/")) d += "/";
  return normalizePath(d + nm);
}
static String modeStr(bool dir) { return dir ? "drwxr-xr-x" : "-rw-r--r--"; }

static uint64_t fileSize(const String &abs) {
  File f = LittleFS.open(abs, "r");
  uint64_t s = f ? f.size() : 0;
  if (f) f.close();
  return s;
}

static uint64_t treeSize(const String &abs) {
  if (!isDir(abs)) return fileSize(abs);
  uint64_t tot = 0;
  std::vector<String> kids;
  File d = LittleFS.open(abs);
  if (d) {
    File e = d.openNextFile();
    while (e) { kids.push_back(childPath(abs, e)); e.close(); e = d.openNextFile(); }
    d.close();
  }
  for (auto &k : kids) tot += treeSize(k);
  return tot;
}

static bool rmrf(const String &abs) {
  if (!isDir(abs)) return LittleFS.remove(abs);
  std::vector<String> kids;
  File d = LittleFS.open(abs);
  if (d) {
    File e = d.openNextFile();
    while (e) { kids.push_back(childPath(abs, e)); e.close(); e = d.openNextFile(); }
    d.close();
  }
  for (auto &k : kids) rmrf(k);
  return LittleFS.rmdir(abs);
}

static bool copyFile(const String &src, const String &dst) {
  File in = LittleFS.open(src, "r");
  if (!in || in.isDirectory()) { if (in) in.close(); return false; }
  File out = LittleFS.open(dst, "w");
  if (!out) { in.close(); return false; }
  uint8_t buf[256];
  while (true) { int n = in.read(buf, sizeof(buf)); if (n <= 0) break; out.write(buf, n); }
  in.close(); out.close();
  return true;
}

static bool copyTree(const String &src, const String &dst) {
  if (!isDir(src)) return copyFile(src, dst);
  LittleFS.mkdir(dst);
  std::vector<String> kids;
  File d = LittleFS.open(src);
  if (d) {
    File e = d.openNextFile();
    while (e) { kids.push_back(childPath(src, e)); e.close(); e = d.openNextFile(); }
    d.close();
  }
  bool ok = true;
  for (auto &k : kids) {
    String dd = dst; if (!dd.endsWith("/")) dd += "/"; dd += leaf(k);
    ok = copyTree(k, dd) && ok;
  }
  return ok;
}

// ============================================================================
//  Commands
// ============================================================================
static int cmd_pwd(int argc, char **argv, ShellIO &io) {
  io.out.println(g_cwd);
  return 0;
}

static int cmd_cd(int argc, char **argv, ShellIO &io) {
  String tgt = argAt(argc, argv, 0);
  if (tgt.length() == 0 || tgt == "~") { g_cwd = "/"; return 0; }
  String abs = resolvePath(tgt);
  if (!isDir(abs)) { io.out.print(tgt); io.out.println(F(": not a directory")); return 1; }
  g_cwd = abs;
  return 0;
}

static int cmd_ls(int argc, char **argv, ShellIO &io) {
  bool lng = hasFlag(argc, argv, 'l');
  bool all = hasFlag(argc, argv, 'a');
  String path = nonFlagCount(argc, argv) ? resolvePath(argAt(argc, argv, 0)) : g_cwd;

  if (!pathExists(path)) { io.out.print(argv[argc - 1]); io.out.println(F(": no such file or directory")); return 1; }
  if (!isDir(path)) {  // listing a single file
    if (lng) { io.out.print(modeStr(false)); io.out.printf(" %8u ", (unsigned)fileSize(path)); }
    io.out.println(baseName(path));
    return 0;
  }

  std::vector<String> kids;
  File d = LittleFS.open(path);
  if (d) {
    File e = d.openNextFile();
    while (e) { kids.push_back(childPath(path, e)); e.close(); e = d.openNextFile(); }
    d.close();
  }
  std::sort(kids.begin(), kids.end());

  for (auto &k : kids) {
    String nm = leaf(k);
    if (!all && nm.startsWith(".")) continue;
    bool dir = isDir(k);
    if (lng) {
      io.out.print(modeStr(dir));
      io.out.printf(" %8u ", dir ? 0u : (unsigned)fileSize(k));
    }
    io.out.print(nm);
    if (dir) io.out.print('/');
    io.out.println();
  }
  return 0;
}

static int cmd_mkdir(int argc, char **argv, ShellIO &io) {
  bool parents = hasFlag(argc, argv, 'p');
  int n = nonFlagCount(argc, argv), rc = 0;
  if (n == 0) { io.out.println(F("mkdir: missing operand")); return 1; }
  for (int i = 0; i < n; ++i) {
    String abs = resolvePath(argAt(argc, argv, i));
    if (parents) {
      String acc = "";
      int from = 1;
      while (from < (int)abs.length()) {
        int sl = abs.indexOf('/', from);
        String part = (sl < 0) ? abs : abs.substring(0, sl);
        if (!pathExists(part)) LittleFS.mkdir(part);
        if (sl < 0) break;
        from = sl + 1;
      }
    } else {
      if (!LittleFS.mkdir(abs)) { io.out.print(argAt(argc, argv, i)); io.out.println(F(": cannot create directory")); rc = 1; }
    }
  }
  return rc;
}

static int cmd_rmdir(int argc, char **argv, ShellIO &io) {
  int n = nonFlagCount(argc, argv), rc = 0;
  for (int i = 0; i < n; ++i) {
    String abs = resolvePath(argAt(argc, argv, i));
    if (!LittleFS.rmdir(abs)) { io.out.print(argAt(argc, argv, i)); io.out.println(F(": failed to remove (not empty or not a dir)")); rc = 1; }
  }
  return rc;
}

static int cmd_touch(int argc, char **argv, ShellIO &io) {
  int n = nonFlagCount(argc, argv), rc = 0;
  for (int i = 0; i < n; ++i) {
    String abs = resolvePath(argAt(argc, argv, i));
    if (!pathExists(abs)) {
      File f = LittleFS.open(abs, "w");
      if (f) f.close();
      else { io.out.print(argAt(argc, argv, i)); io.out.println(F(": cannot create")); rc = 1; }
    }
  }
  return rc;
}

static int cmd_rm(int argc, char **argv, ShellIO &io) {
  bool recur = hasFlag(argc, argv, 'r') || hasFlag(argc, argv, 'R');
  bool force = hasFlag(argc, argv, 'f');
  int n = nonFlagCount(argc, argv), rc = 0;
  if (n == 0) { io.out.println(F("rm: missing operand")); return 1; }
  for (int i = 0; i < n; ++i) {
    String abs = resolvePath(argAt(argc, argv, i));
    if (!pathExists(abs)) { if (!force) { io.out.print(argAt(argc, argv, i)); io.out.println(F(": no such file")); rc = 1; } continue; }
    if (isDir(abs)) {
      if (!recur) { io.out.print(argAt(argc, argv, i)); io.out.println(F(": is a directory (use -r)")); rc = 1; continue; }
      if (!rmrf(abs)) rc = 1;
    } else {
      if (!LittleFS.remove(abs)) rc = 1;
    }
  }
  return rc;
}

static int cmd_cp(int argc, char **argv, ShellIO &io) {
  bool recur = hasFlag(argc, argv, 'r') || hasFlag(argc, argv, 'R');
  int n = nonFlagCount(argc, argv);
  if (n < 2) { io.out.println(F("usage: cp [-r] src dst")); return 1; }
  String src = resolvePath(argAt(argc, argv, 0));
  String dst = resolvePath(argAt(argc, argv, 1));
  if (!pathExists(src)) { io.out.print(argAt(argc, argv, 0)); io.out.println(F(": no such file")); return 1; }
  if (isDir(dst)) { String d = dst; if (!d.endsWith("/")) d += "/"; dst = normalizePath(d + baseName(src)); }
  if (isDir(src)) {
    if (!recur) { io.out.println(F("cp: source is a directory (use -r)")); return 1; }
    return copyTree(src, dst) ? 0 : 1;
  }
  return copyFile(src, dst) ? 0 : (io.out.println(F("cp: copy failed")), 1);
}

static int cmd_mv(int argc, char **argv, ShellIO &io) {
  int n = nonFlagCount(argc, argv);
  if (n < 2) { io.out.println(F("usage: mv src dst")); return 1; }
  String src = resolvePath(argAt(argc, argv, 0));
  String dst = resolvePath(argAt(argc, argv, 1));
  if (isDir(dst)) { String d = dst; if (!d.endsWith("/")) d += "/"; dst = normalizePath(d + baseName(src)); }
  if (!LittleFS.rename(src, dst)) { io.out.println(F("mv: rename failed")); return 1; }
  return 0;
}

static int cmd_file(int argc, char **argv, ShellIO &io) {
  int n = nonFlagCount(argc, argv), rc = 0;
  for (int i = 0; i < n; ++i) {
    String name = argAt(argc, argv, i);
    String abs = resolvePath(name);
    io.out.print(name); io.out.print(F(": "));
    if (!pathExists(abs)) { io.out.println(F("cannot open (no such file)")); rc = 1; continue; }
    if (isDir(abs)) { io.out.println(F("directory")); continue; }
    File f = LittleFS.open(abs, "r");
    if (!f) { io.out.println(F("cannot open")); rc = 1; continue; }
    size_t total = f.size();
    bool text = true;
    size_t look = total < 256 ? total : 256;
    for (size_t k = 0; k < look; ++k) {
      int c = f.read();
      if (c == '\n' || c == '\r' || c == '\t') continue;
      if (c < 0x20 || c > 0x7e) { text = false; break; }
    }
    f.close();
    if (total == 0) io.out.println(F("empty"));
    else io.out.println(text ? F("ASCII text") : F("data"));
  }
  return rc;
}

static int cmd_stat(int argc, char **argv, ShellIO &io) {
  int n = nonFlagCount(argc, argv), rc = 0;
  for (int i = 0; i < n; ++i) {
    String name = argAt(argc, argv, i);
    String abs = resolvePath(name);
    if (!pathExists(abs)) { io.out.print(F("stat: ")); io.out.print(name); io.out.println(F(": no such file")); rc = 1; continue; }
    bool dir = isDir(abs);
    io.out.print(F("  File: ")); io.out.println(abs);
    io.out.print(F("  Size: ")); io.out.print((unsigned)(dir ? 0 : fileSize(abs)));
    io.out.print(F("   Type: ")); io.out.println(dir ? F("directory") : F("regular file"));
    if (!dir) {
      File f = LittleFS.open(abs, "r");
      if (f) { time_t t = f.getLastWrite(); if (t > 0) { io.out.print(F("  Mtime: ")); io.out.println((long)t); } f.close(); }
    }
  }
  return rc;
}

static int cmd_basename(int argc, char **argv, ShellIO &io) {
  if (nonFlagCount(argc, argv) < 1) { io.out.println(F("usage: basename path [suffix]")); return 1; }
  String b = baseName(resolvePath(argAt(argc, argv, 0)));
  String suf = argAt(argc, argv, 1);
  if (suf.length() && b.endsWith(suf) && b != suf) b = b.substring(0, b.length() - suf.length());
  io.out.println(b);
  return 0;
}

static int cmd_dirname(int argc, char **argv, ShellIO &io) {
  if (nonFlagCount(argc, argv) < 1) { io.out.println(F("usage: dirname path")); return 1; }
  io.out.println(dirName(resolvePath(argAt(argc, argv, 0))));
  return 0;
}

static int cmd_realpath(int argc, char **argv, ShellIO &io) {
  if (nonFlagCount(argc, argv) < 1) { io.out.println(g_cwd); return 0; }
  io.out.println(resolvePath(argAt(argc, argv, 0)));
  return 0;
}

static void treeWalk(const String &abs, String prefix, ShellIO &io, int &nd, int &nf) {
  std::vector<String> kids;
  File d = LittleFS.open(abs);
  if (d) {
    File e = d.openNextFile();
    while (e) { kids.push_back(childPath(abs, e)); e.close(); e = d.openNextFile(); }
    d.close();
  }
  std::sort(kids.begin(), kids.end());
  for (size_t i = 0; i < kids.size(); ++i) {
    bool last = (i + 1 == kids.size());
    bool dir = isDir(kids[i]);
    io.out.print(prefix);
    io.out.print(last ? F("`-- ") : F("|-- "));
    io.out.print(leaf(kids[i]));
    if (dir) { io.out.println('/'); nd++; treeWalk(kids[i], prefix + (last ? "    " : "|   "), io, nd, nf); }
    else { io.out.println(); nf++; }
  }
}

static int cmd_tree(int argc, char **argv, ShellIO &io) {
  String root = nonFlagCount(argc, argv) ? resolvePath(argAt(argc, argv, 0)) : g_cwd;
  if (!isDir(root)) { io.out.print(root); io.out.println(F(": not a directory")); return 1; }
  io.out.println(root);
  int nd = 0, nf = 0;
  treeWalk(root, "", io, nd, nf);
  io.out.printf("\n%d directories, %d files\n", nd, nf);
  return 0;
}

static int cmd_du(int argc, char **argv, ShellIO &io) {
  bool human = hasFlag(argc, argv, 'h');
  String path = nonFlagCount(argc, argv) ? resolvePath(argAt(argc, argv, 0)) : g_cwd;
  uint64_t sz = treeSize(path);
  if (human) io.out.print(humanBytes(sz));
  else io.out.print((unsigned long)sz);
  io.out.print('\t');
  io.out.println(path);
  return 0;
}

static int cmd_df(int argc, char **argv, ShellIO &io) {
  bool human = hasFlag(argc, argv, 'h');
  uint64_t total = LittleFS.totalBytes();
  uint64_t used = LittleFS.usedBytes();
  uint64_t avail = total > used ? total - used : 0;
  int usePct = total ? (int)((used * 100) / total) : 0;
  io.out.println(F("Filesystem       Size     Used    Avail  Use%  Mounted"));
  io.out.print(F("LittleFS   "));
  if (human) io.out.printf(" %8s %8s %8s  %3d%%  /\n",
                           humanBytes(total).c_str(), humanBytes(used).c_str(), humanBytes(avail).c_str(), usePct);
  else io.out.printf(" %8lu %8lu %8lu  %3d%%  /\n",
                     (unsigned long)total, (unsigned long)used, (unsigned long)avail, usePct);
  return 0;
}

static int cmd_ln(int argc, char **argv, ShellIO &io) {
  io.out.println(F("ln: LittleFS has no symbolic/hard links (unsupported)"));
  return 1;
}

static int cmd_noperm(int argc, char **argv, ShellIO &io) {
  io.out.print(argv[0]);
  io.out.println(F(": LittleFS has no permission/ownership model (no-op)"));
  return 0;
}

const Command FS_CMDS[] = {
  {"pwd",      cmd_pwd,      "pwd",                 "print working directory",             G_FS},
  {"cd",       cmd_cd,       "cd [dir]",            "change directory",                    G_FS},
  {"ls",       cmd_ls,       "ls [-l] [-a] [path]", "list directory contents",             G_FS},
  {"mkdir",    cmd_mkdir,    "mkdir [-p] dir...",   "create directories",                  G_FS},
  {"rmdir",    cmd_rmdir,    "rmdir dir...",        "remove empty directories",            G_FS},
  {"touch",    cmd_touch,    "touch file...",       "create empty files",                  G_FS},
  {"cp",       cmd_cp,       "cp [-r] src dst",     "copy files or directories",           G_FS},
  {"mv",       cmd_mv,       "mv src dst",          "move / rename",                       G_FS},
  {"rm",       cmd_rm,       "rm [-r] [-f] file...","remove files / directories",          G_FS},
  {"file",     cmd_file,     "file path...",        "identify file type",                  G_FS},
  {"stat",     cmd_stat,     "stat path...",        "show file status",                    G_FS},
  {"basename", cmd_basename, "basename path [suf]", "strip directory (and suffix)",        G_FS},
  {"dirname",  cmd_dirname,  "dirname path",        "strip last component",                G_FS},
  {"realpath", cmd_realpath, "realpath path",       "resolve to an absolute path",         G_FS},
  {"tree",     cmd_tree,     "tree [path]",         "recursive directory listing",         G_FS},
  {"du",       cmd_du,       "du [-h] [path]",      "disk usage of a tree",                G_FS},
  {"df",       cmd_df,       "df [-h]",             "filesystem space usage",              G_FS},
  {"ln",       cmd_ln,       "ln target link",      "(unsupported on LittleFS)",           G_FS},
  {"chmod",    cmd_noperm,   "chmod mode file",     "(no-op: no permissions on LittleFS)", G_FS},
  {"chown",    cmd_noperm,   "chown user file",     "(no-op: no ownership on LittleFS)",   G_FS},
  {"chgrp",    cmd_noperm,   "chgrp group file",    "(no-op: no ownership on LittleFS)",   G_FS},
};
const size_t FS_CMDS_N = sizeof(FS_CMDS) / sizeof(FS_CMDS[0]);
