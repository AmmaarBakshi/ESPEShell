#include "shell.h"
#include <LittleFS.h>
#include <algorithm>
#include "esp_random.h"

// ============================================================================
//  Text-processing commands
// ============================================================================

// Collect input from a piped stage, else from an explicit list of file names.
static bool getInput(ShellIO &io, const std::vector<String> &files, String &out) {
  out = "";
  if (io.hasIn()) { out = *io.in; return true; }
  bool any = false;
  for (auto &fn : files) {
    String chunk;
    if (!readFileToString(resolvePath(fn), chunk, &io.out, fn.c_str())) continue;
    out += chunk;
    any = true;
  }
  return any;
}

// Gather non-flag args as file operands.
static void fileArgs(int argc, char **argv, std::vector<String> &files) {
  for (int i = 1; i < argc; ++i)
    if (!(argv[i][0] == '-' && argv[i][1] != 0)) files.push_back(String(argv[i]));
}
static bool hasOpt(int argc, char **argv, char f) {
  for (int i = 1; i < argc; ++i)
    if (argv[i][0] == '-' && argv[i][1] != '-')
      for (const char *p = argv[i] + 1; *p; ++p) if (*p == f) return true;
  return false;
}

// ---- cat / more / less -----------------------------------------------------
static int cmd_cat(int argc, char **argv, ShellIO &io) {
  bool number = hasOpt(argc, argv, 'n');
  std::vector<String> files; fileArgs(argc, argv, files);
  String data;
  if (!getInput(io, files, data)) return 0;
  if (!number) { io.out.print(data); return 0; }
  std::vector<String> lines; splitLines(data, lines);
  for (size_t i = 0; i < lines.size(); ++i) io.out.printf("%6u\t%s\n", (unsigned)(i + 1), lines[i].c_str());
  return 0;
}

// ---- tac -------------------------------------------------------------------
static int cmd_tac(int argc, char **argv, ShellIO &io) {
  std::vector<String> files; fileArgs(argc, argv, files);
  String data;
  if (!getInput(io, files, data)) return 0;
  std::vector<String> lines; splitLines(data, lines);
  for (size_t i = lines.size(); i-- > 0; ) io.out.println(lines[i]);
  return 0;
}

// ---- rev -------------------------------------------------------------------
static int cmd_rev(int argc, char **argv, ShellIO &io) {
  std::vector<String> files; fileArgs(argc, argv, files);
  String data;
  if (!getInput(io, files, data)) return 0;
  std::vector<String> lines; splitLines(data, lines);
  for (auto &l : lines) {
    String r; r.reserve(l.length());
    for (int i = (int)l.length(); i-- > 0; ) r += l[i];
    io.out.println(r);
  }
  return 0;
}

// ---- shuf ------------------------------------------------------------------
// Fisher-Yates over the input lines, seeded from the hardware RNG.
static int cmd_shuf(int argc, char **argv, ShellIO &io) {
  int count = -1;
  std::vector<String> files;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-n" && i + 1 < argc) count = atoi(argv[++i]);
    else if (a.length() > 1 && a[0] == '-' && isdigit((int)a[1])) count = atoi(a.c_str() + 1);
    else if (a[0] != '-') files.push_back(a);
  }
  String data;
  if (!getInput(io, files, data)) return 0;
  std::vector<String> lines; splitLines(data, lines);
  for (size_t i = lines.size(); i > 1; --i)
    std::swap(lines[i - 1], lines[esp_random() % i]);
  size_t n = lines.size();
  if (count >= 0 && (size_t)count < n) n = (size_t)count;
  for (size_t i = 0; i < n; ++i) io.out.println(lines[i]);
  return 0;
}

// ---- head / tail -----------------------------------------------------------
static int headTail(int argc, char **argv, ShellIO &io, bool head) {
  int count = 10;
  std::vector<String> files;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-n" && i + 1 < argc) { count = atoi(argv[++i]); }
    else if (a.length() > 1 && a[0] == '-' && isdigit((int)a[1])) { count = atoi(a.c_str() + 1); }
    else if (a[0] != '-') files.push_back(a);
  }
  String data; if (!getInput(io, files, data)) return 0;
  std::vector<String> lines; splitLines(data, lines);
  int n = lines.size();
  if (head) {
    for (int i = 0; i < n && i < count; ++i) io.out.println(lines[i]);
  } else {
    int start = n - count; if (start < 0) start = 0;
    for (int i = start; i < n; ++i) io.out.println(lines[i]);
  }
  return 0;
}
static int cmd_head(int argc, char **argv, ShellIO &io) { return headTail(argc, argv, io, true); }
static int cmd_tail(int argc, char **argv, ShellIO &io) { return headTail(argc, argv, io, false); }

// ---- wc --------------------------------------------------------------------
static int cmd_wc(int argc, char **argv, ShellIO &io) {
  bool wl = hasOpt(argc, argv, 'l'), ww = hasOpt(argc, argv, 'w'), wc_ = hasOpt(argc, argv, 'c');
  bool any = wl || ww || wc_;
  std::vector<String> files; fileArgs(argc, argv, files);
  String data; getInput(io, files, data);
  unsigned lines = 0, words = 0, chars = data.length();
  bool inWord = false;
  for (size_t i = 0; i < data.length(); ++i) {
    char c = data[i];
    if (c == '\n') lines++;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') inWord = false;
    else if (!inWord) { inWord = true; words++; }
  }
  if (!any) { io.out.printf("%7u %7u %7u\n", lines, words, chars); return 0; }
  bool first = true;
  if (wl) { io.out.printf("%7u", lines); first = false; }
  if (ww) { io.out.printf(first ? "%7u" : " %7u", words); first = false; }
  if (wc_) { io.out.printf(first ? "%7u" : " %7u", chars); }
  io.out.println();
  return 0;
}

// ---- sort ------------------------------------------------------------------
static int cmd_sort(int argc, char **argv, ShellIO &io) {
  bool rev = hasOpt(argc, argv, 'r'), num = hasOpt(argc, argv, 'n'), uniq = hasOpt(argc, argv, 'u');
  std::vector<String> files; fileArgs(argc, argv, files);
  String data; getInput(io, files, data);
  std::vector<String> lines; splitLines(data, lines);
  std::sort(lines.begin(), lines.end(), [num](const String &a, const String &b) {
    if (num) { long x = atol(a.c_str()), y = atol(b.c_str()); if (x != y) return x < y; }
    return a < b;
  });
  if (rev) std::reverse(lines.begin(), lines.end());
  String prev; bool havePrev = false;
  for (auto &l : lines) {
    if (uniq && havePrev && l == prev) continue;
    io.out.println(l);
    prev = l; havePrev = true;
  }
  return 0;
}

// ---- uniq ------------------------------------------------------------------
static int cmd_uniq(int argc, char **argv, ShellIO &io) {
  bool count = hasOpt(argc, argv, 'c'), dupOnly = hasOpt(argc, argv, 'd');
  std::vector<String> files; fileArgs(argc, argv, files);
  String data; getInput(io, files, data);
  std::vector<String> lines; splitLines(data, lines);
  size_t i = 0;
  while (i < lines.size()) {
    size_t j = i + 1;
    while (j < lines.size() && lines[j] == lines[i]) j++;
    unsigned c = j - i;
    if (!dupOnly || c > 1) {
      if (count) io.out.printf("%4u %s\n", c, lines[i].c_str());
      else io.out.println(lines[i]);
    }
    i = j;
  }
  return 0;
}

// ---- cut -------------------------------------------------------------------
static int cmd_cut(int argc, char **argv, ShellIO &io) {
  char delim = '\t';
  String fields, chars;
  std::vector<String> files;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a.startsWith("-d")) { String v = a.length() > 2 ? a.substring(2) : (i + 1 < argc ? String(argv[++i]) : String("")); if (v.length()) delim = v[0]; }
    else if (a.startsWith("-f")) { fields = a.length() > 2 ? a.substring(2) : (i + 1 < argc ? String(argv[++i]) : String("")); }
    else if (a.startsWith("-c")) { chars = a.length() > 2 ? a.substring(2) : (i + 1 < argc ? String(argv[++i]) : String("")); }
    else if (a[0] != '-') files.push_back(a);
  }
  String data; getInput(io, files, data);
  std::vector<String> lines; splitLines(data, lines);

  // parse a spec like "1,3-4" into a lambda test(idx) 1-based
  auto inSpec = [](const String &spec, int idx) -> bool {
    int i = 0, n = spec.length();
    while (i < n) {
      int j = i; while (j < n && spec[j] != ',') j++;
      String part = spec.substring(i, j);
      int dash = part.indexOf('-');
      if (dash < 0) { if (part.toInt() == idx) return true; }
      else {
        int lo = part.substring(0, dash).toInt();
        String hiS = part.substring(dash + 1);
        int hi = hiS.length() ? hiS.toInt() : 1000000;
        if (idx >= lo && idx <= hi) return true;
      }
      i = j + 1;
    }
    return false;
  };

  for (auto &line : lines) {
    if (chars.length()) {
      String o;
      for (int k = 0; k < (int)line.length(); ++k) if (inSpec(chars, k + 1)) o += line[k];
      io.out.println(o);
    } else if (fields.length()) {
      // split by delim
      std::vector<String> cols; String cur;
      for (int k = 0; k < (int)line.length(); ++k) { if (line[k] == delim) { cols.push_back(cur); cur = ""; } else cur += line[k]; }
      cols.push_back(cur);
      String o; bool first = true;
      for (int c = 0; c < (int)cols.size(); ++c) if (inSpec(fields, c + 1)) { if (!first) o += delim; o += cols[c]; first = false; }
      io.out.println(o);
    } else {
      io.out.println(line);
    }
  }
  return 0;
}

// ---- tr --------------------------------------------------------------------
// Expands "a-z" style ranges into the literal characters they cover.
static String expandSet(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); ++i) {
    if (i + 2 < s.length() && s[i + 1] == '-' && s[i + 2] >= s[i]) {
      for (char c = s[i]; c <= s[i + 2]; ++c) o += c;
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

static int cmd_tr(int argc, char **argv, ShellIO &io) {
  bool del = hasOpt(argc, argv, 'd');
  std::vector<String> ops;
  for (int i = 1; i < argc; ++i) if (!(argv[i][0] == '-' && argv[i][1] != 0)) ops.push_back(argv[i]);
  String set1 = expandSet(ops.size() > 0 ? ops[0] : String(""));
  String set2 = expandSet(ops.size() > 1 ? ops[1] : String(""));
  if (!io.hasIn()) {                       // tr reads only stdin
    io.out.println(F("tr: no input (use a pipe, or '< file')"));
    return 1;
  }
  String data = *io.in;
  String out;
  for (size_t i = 0; i < data.length(); ++i) {
    char c = data[i];
    int idx = set1.indexOf(c);
    if (idx >= 0) {
      if (del) continue;
      if (set2.length()) out += set2[idx < (int)set2.length() ? idx : set2.length() - 1];
      else out += c;
    } else out += c;
  }
  io.out.print(out);
  return 0;
}

// ---- tee -------------------------------------------------------------------
static int cmd_tee(int argc, char **argv, ShellIO &io) {
  bool append = hasOpt(argc, argv, 'a');
  String data = io.hasIn() ? *io.in : "";
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-' && argv[i][1] != 0) continue;
    String ap = resolvePath(argv[i]);
    File f = LittleFS.open(ap, append ? "a" : "w");
    if (f) { f.print(toUnixEol(data)); f.close(); }
    else { io.out.print(argv[i]); io.out.println(F(": cannot open")); }
  }
  io.out.print(data);
  return 0;
}

// ---- nl --------------------------------------------------------------------
static int cmd_nl(int argc, char **argv, ShellIO &io) {
  std::vector<String> files; fileArgs(argc, argv, files);
  String data; getInput(io, files, data);
  std::vector<String> lines; splitLines(data, lines);
  unsigned n = 1;
  for (auto &l : lines) {
    if (l.length()) io.out.printf("%6u\t%s\n", n++, l.c_str());
    else io.out.println();
  }
  return 0;
}

// ---- fold ------------------------------------------------------------------
static int cmd_fold(int argc, char **argv, ShellIO &io) {
  int w = 80;
  std::vector<String> files;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-w" && i + 1 < argc) w = atoi(argv[++i]);
    else if (a.startsWith("-w")) w = atoi(a.c_str() + 2);
    else if (a[0] != '-') files.push_back(a);
  }
  if (w < 1) w = 1;
  String data; getInput(io, files, data);
  std::vector<String> lines; splitLines(data, lines);
  for (auto &l : lines) {
    if (l.length() == 0) { io.out.println(); continue; }
    for (int i = 0; i < (int)l.length(); i += w) io.out.println(l.substring(i, i + w));
  }
  return 0;
}

// ---- fmt -------------------------------------------------------------------
static int cmd_fmt(int argc, char **argv, ShellIO &io) {
  int w = 75;
  std::vector<String> files;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-w" && i + 1 < argc) w = atoi(argv[++i]);
    else if (a.startsWith("-w")) w = atoi(a.c_str() + 2);
    else if (a[0] != '-') files.push_back(a);
  }
  if (w < 1) w = 1;
  String data; getInput(io, files, data);
  // split into words on whitespace, reflow
  String line;
  String word;
  auto flushWord = [&](void) {
    if (!word.length()) return;
    if (line.length() == 0) line = word;
    else if (line.length() + 1 + word.length() <= (size_t)w) { line += ' '; line += word; }
    else { io.out.println(line); line = word; }
    word = "";
  };
  for (size_t i = 0; i <= data.length(); ++i) {
    char c = (i < data.length()) ? data[i] : ' ';
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') flushWord();
    else word += c;
  }
  if (line.length()) io.out.println(line);
  return 0;
}

// ---- od --------------------------------------------------------------------
static int cmd_od(int argc, char **argv, ShellIO &io) {
  bool chars = hasOpt(argc, argv, 'c');
  bool hex = hasOpt(argc, argv, 'x');
  std::vector<String> files; fileArgs(argc, argv, files);
  String data; getInput(io, files, data);
  size_t off = 0;
  while (off < data.length()) {
    io.out.printf("%07o ", (unsigned)off);
    size_t line = data.length() - off; if (line > 16) line = 16;
    for (size_t k = 0; k < line; ++k) {
      unsigned char c = data[off + k];
      if (chars) {
        if (c == '\n') io.out.print(" \\n");
        else if (c == '\t') io.out.print(" \\t");
        else if (c >= 0x20 && c < 0x7f) io.out.printf("  %c", c);
        else io.out.printf(" %03o", c);
      } else if (hex) io.out.printf(" %02x", c);
      else io.out.printf(" %03o", c);
    }
    io.out.println();
    off += line;
  }
  io.out.printf("%07o\n", (unsigned)data.length());
  return 0;
}

// ---- xxd -------------------------------------------------------------------
static int cmd_xxd(int argc, char **argv, ShellIO &io) {
  std::vector<String> files; fileArgs(argc, argv, files);
  String data; getInput(io, files, data);
  size_t off = 0;
  while (off < data.length()) {
    io.out.printf("%08x: ", (unsigned)off);
    size_t line = data.length() - off; if (line > 16) line = 16;
    for (size_t k = 0; k < 16; ++k) {
      if (k < line) io.out.printf("%02x", (unsigned char)data[off + k]);
      else io.out.print("  ");
      if (k % 2) io.out.print(' ');
    }
    io.out.print(' ');
    for (size_t k = 0; k < line; ++k) {
      unsigned char c = data[off + k];
      io.out.print((c >= 0x20 && c < 0x7f) ? (char)c : '.');
    }
    io.out.println();
    off += line;
  }
  return 0;
}

// ---- strings ---------------------------------------------------------------
static int cmd_strings(int argc, char **argv, ShellIO &io) {
  int minlen = 4;
  std::vector<String> files;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-n" && i + 1 < argc) minlen = atoi(argv[++i]);
    else if (a[0] != '-') files.push_back(a);
  }
  String data; getInput(io, files, data);
  String run;
  for (size_t i = 0; i <= data.length(); ++i) {
    char c = (i < data.length()) ? data[i] : 0;
    if (c >= 0x20 && c < 0x7f) run += c;
    else { if ((int)run.length() >= minlen) io.out.println(run); run = ""; }
  }
  return 0;
}

// ---- diff ------------------------------------------------------------------
static int cmd_diff(int argc, char **argv, ShellIO &io) {
  std::vector<String> files; fileArgs(argc, argv, files);
  if (files.size() < 2) { io.out.println(F("usage: diff file1 file2")); return 2; }
  String a, b;
  { std::vector<String> f1{files[0]}, f2{files[1]}; ShellIO na(io.out, nullptr); getInput(na, f1, a); getInput(na, f2, b); }
  std::vector<String> A, B; splitLines(a, A); splitLines(b, B);
  int n = A.size(), m = B.size();
  if ((long)n * (long)m > 6000) {  // guard memory: fall back to plain compare
    int mx = n > m ? n : m;
    bool same = true;
    for (int i = 0; i < mx; ++i) {
      String la = i < n ? A[i] : String("");
      String lb = i < m ? B[i] : String("");
      if (la != lb) { io.out.print(F("< ")); io.out.println(la); io.out.print(F("> ")); io.out.println(lb); same = false; }
    }
    return same ? 0 : 1;
  }
  std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
  for (int i = n - 1; i >= 0; --i)
    for (int j = m - 1; j >= 0; --j)
      dp[i][j] = (A[i] == B[j]) ? dp[i + 1][j + 1] + 1 : std::max(dp[i + 1][j], dp[i][j + 1]);
  int i = 0, j = 0, diffs = 0;
  while (i < n && j < m) {
    if (A[i] == B[j]) { i++; j++; }
    else if (dp[i + 1][j] >= dp[i][j + 1]) { io.out.print(F("< ")); io.out.println(A[i++]); diffs++; }
    else { io.out.print(F("> ")); io.out.println(B[j++]); diffs++; }
  }
  while (i < n) { io.out.print(F("< ")); io.out.println(A[i++]); diffs++; }
  while (j < m) { io.out.print(F("> ")); io.out.println(B[j++]); diffs++; }
  return diffs ? 1 : 0;
}

// ---- cmp -------------------------------------------------------------------
static int cmd_cmp(int argc, char **argv, ShellIO &io) {
  std::vector<String> files; fileArgs(argc, argv, files);
  if (files.size() < 2) { io.out.println(F("usage: cmp file1 file2")); return 2; }
  String a, b;
  { std::vector<String> f1{files[0]}, f2{files[1]}; ShellIO na(io.out, nullptr); getInput(na, f1, a); getInput(na, f2, b); }
  size_t n = std::min(a.length(), b.length());
  unsigned line = 1;
  for (size_t k = 0; k < n; ++k) {
    if (a[k] != b[k]) {
      io.out.printf("%s %s differ: byte %u, line %u\n", files[0].c_str(), files[1].c_str(), (unsigned)(k + 1), line);
      return 1;
    }
    if (a[k] == '\n') line++;
  }
  if (a.length() != b.length()) {
    io.out.printf("cmp: EOF on %s\n", (a.length() < b.length() ? files[0].c_str() : files[1].c_str()));
    return 1;
  }
  return 0;
}

// ---- paste -----------------------------------------------------------------
static int cmd_paste(int argc, char **argv, ShellIO &io) {
  char delim = '\t';
  std::vector<String> files;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a.startsWith("-d")) { String v = a.length() > 2 ? a.substring(2) : (i + 1 < argc ? String(argv[++i]) : String("")); if (v.length()) delim = v[0]; }
    else if (a[0] != '-') files.push_back(a);
  }
  if (io.hasIn() && files.empty()) { io.out.print(*io.in); return 0; }
  std::vector<std::vector<String>> cols;
  size_t maxLines = 0;
  for (auto &fn : files) {
    String d; std::vector<String> f{fn}; ShellIO na(io.out, nullptr); getInput(na, f, d);
    std::vector<String> ls; splitLines(d, ls);
    if (ls.size() > maxLines) maxLines = ls.size();
    cols.push_back(ls);
  }
  for (size_t r = 0; r < maxLines; ++r) {
    for (size_t c = 0; c < cols.size(); ++c) {
      if (c) io.out.print(delim);
      if (r < cols[c].size()) io.out.print(cols[c][r]);
    }
    io.out.println();
  }
  return 0;
}

const Command TEXT_CMDS[] = {
  {"cat",     cmd_cat,     "cat [-n] [file...]",   "concatenate / print files",         G_TEXT},
  {"more",    cmd_cat,     "more [file]",          "page through text (no pager: cat)", G_TEXT},
  {"less",    cmd_cat,     "less [file]",          "page through text (no pager: cat)", G_TEXT},
  {"tac",     cmd_tac,     "tac [file...]",        "print lines in reverse order",      G_TEXT},
  {"rev",     cmd_rev,     "rev [file...]",        "reverse the characters of each line",G_TEXT},
  {"shuf",    cmd_shuf,    "shuf [-n N] [file...]","shuffle lines randomly",            G_TEXT},
  {"head",    cmd_head,    "head [-n N] [file]",   "first N lines (default 10)",        G_TEXT},
  {"tail",    cmd_tail,    "tail [-n N] [file]",   "last N lines (default 10)",         G_TEXT},
  {"wc",      cmd_wc,      "wc [-lwc] [file]",     "count lines, words, characters",    G_TEXT},
  {"sort",    cmd_sort,    "sort [-rnu] [file]",   "sort lines",                        G_TEXT},
  {"uniq",    cmd_uniq,    "uniq [-cd] [file]",    "collapse adjacent duplicate lines", G_TEXT},
  {"cut",     cmd_cut,     "cut -f LIST [-d C]",   "select fields / characters",        G_TEXT},
  {"paste",   cmd_paste,   "paste [-d C] file...", "merge lines of files",              G_TEXT},
  {"tr",      cmd_tr,      "tr [-d] SET1 [SET2]",  "translate or delete characters",    G_TEXT},
  {"tee",     cmd_tee,     "tee [-a] file...",     "copy stdin to files and stdout",    G_TEXT},
  {"nl",      cmd_nl,      "nl [file]",            "number non-empty lines",            G_TEXT},
  {"fold",    cmd_fold,    "fold [-w N] [file]",   "wrap lines to width N",             G_TEXT},
  {"fmt",     cmd_fmt,     "fmt [-w N] [file]",    "reflow text to width N",            G_TEXT},
  {"od",      cmd_od,      "od [-c|-x] [file]",    "octal / hex dump",                  G_TEXT},
  {"xxd",     cmd_xxd,     "xxd [file]",           "hex + ASCII dump",                  G_TEXT},
  {"strings", cmd_strings, "strings [-n N] [file]","printable strings in a file",       G_TEXT},
  {"diff",    cmd_diff,    "diff file1 file2",     "line differences (<,>)",            G_TEXT},
  {"cmp",     cmd_cmp,     "cmp file1 file2",      "byte-compare two files",            G_TEXT},
};
const size_t TEXT_CMDS_N = sizeof(TEXT_CMDS) / sizeof(TEXT_CMDS[0]);
