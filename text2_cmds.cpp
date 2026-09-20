#include "shell.h"
#include <LittleFS.h>
#include <algorithm>

// ============================================================================
//  Text utilities, part two - the line-and-column tools.
//
//  Argument handling goes through ArgScan so that a flag's value is never
//  mistaken for an operand: `truncate -s 100 file` has one operand, and
//  `expand -t 4 file` reads one file rather than trying to open "4".
// ============================================================================

// Loads one file's lines. Reports through `io` and returns false on failure.
static bool fileLines(const String &name, ShellIO &io, std::vector<String> &out) {
  String data;
  if (!readFileToString(resolvePath(name), data, &io.out, name.c_str())) return false;
  splitLines(data, out);
  return true;
}

// ---- comm ------------------------------------------------------------------
// Three columns: only in file1, only in file2, in both. Like the real thing,
// this assumes both inputs are already sorted - it is a merge, not a diff.
static int cmd_comm(int argc, char **argv, ShellIO &io) {
  ArgScan args(argc, argv);
  if (args.count() < 2) {
    io.out.println(F("usage: comm [-123] file1 file2   (both must be sorted)"));
    return 1;
  }
  std::vector<String> a, b;
  if (!fileLines(args.at(0), io, a) || !fileLines(args.at(1), io, b)) return 1;

  const bool hide1 = args.letter('1');
  const bool hide2 = args.letter('2');
  const bool hide3 = args.letter('3');
  // Column N is indented by one tab for each shown column before it.
  const String pad2 = hide1 ? "" : "\t";
  const String pad3 = String(hide1 ? "" : "\t") + (hide2 ? "" : "\t");

  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    if (j >= b.size() || (i < a.size() && a[i] < b[j])) {
      if (!hide1) io.out.println(a[i]);
      i++;
    } else if (i >= a.size() || b[j] < a[i]) {
      if (!hide2) { io.out.print(pad2); io.out.println(b[j]); }
      j++;
    } else {
      if (!hide3) { io.out.print(pad3); io.out.println(a[i]); }
      i++; j++;
    }
  }
  return 0;
}

// ---- join ------------------------------------------------------------------
static int cmd_join(int argc, char **argv, ShellIO &io) {
  ArgScan args(argc, argv, "-t");
  if (args.count() < 2) {
    io.out.println(F("usage: join [-t C] file1 file2   (joins on the first field)"));
    return 1;
  }
  const String sepArg = args.value("-t", " ");
  const char sep = sepArg.length() ? sepArg[0] : ' ';

  std::vector<String> a, b;
  if (!fileLines(args.at(0), io, a) || !fileLines(args.at(1), io, b)) return 1;

  for (auto &la : a) {
    const int ka = la.indexOf(sep);
    const String key   = (ka < 0) ? la : la.substring(0, ka);
    const String resta = (ka < 0) ? String("") : la.substring(ka + 1);
    for (auto &lb : b) {
      const int kb = lb.indexOf(sep);
      if (((kb < 0) ? lb : lb.substring(0, kb)) != key) continue;
      const String restb = (kb < 0) ? String("") : lb.substring(kb + 1);
      io.out.print(key);
      if (resta.length()) { io.out.print(sep); io.out.print(resta); }
      if (restb.length()) { io.out.print(sep); io.out.print(restb); }
      io.out.println();
    }
  }
  return 0;
}

// ---- expand / unexpand -----------------------------------------------------
static int expandTabs(int argc, char **argv, ShellIO &io, bool toSpaces) {
  ArgScan args(argc, argv, "-t");
  long width = args.number("-t", 8);
  if (width < 1) width = 8;

  String data;
  if (!collectOperands(args, 0, io, data)) return 0;
  std::vector<String> lines;
  splitLines(data, lines);

  for (auto &line : lines) {
    String out;
    out.reserve(line.length() + 8);
    if (toSpaces) {
      for (unsigned i = 0; i < line.length(); ++i) {
        if (line[i] != '\t') { out.concat(line[i]); continue; }
        // A tab advances to the next multiple of `width`, not by `width`.
        long pad = width - (out.length() % width);
        while (pad-- > 0) out.concat(' ');
      }
    } else {
      // Only leading whitespace is converted; runs inside a line are data.
      unsigned i = 0;
      long col = 0;
      while (i < line.length() && (line[i] == ' ' || line[i] == '\t')) {
        col = (line[i] == '\t') ? (col / width + 1) * width : col + 1;
        i++;
      }
      for (long t = 0; t < col / width; ++t) out.concat('\t');
      for (long sp = 0; sp < col % width; ++sp) out.concat(' ');
      out.concat(line.substring(i));
    }
    io.out.println(out);
  }
  return 0;
}

static int cmd_expand(int argc, char **argv, ShellIO &io)   { return expandTabs(argc, argv, io, true); }
static int cmd_unexpand(int argc, char **argv, ShellIO &io) { return expandTabs(argc, argv, io, false); }

// ---- column ----------------------------------------------------------------
// Aligns whitespace- (or -s) separated fields into columns, sized to content.
static int cmd_column(int argc, char **argv, ShellIO &io) {
  ArgScan args(argc, argv, "-s");
  const String sepArg = args.value("-s");
  const char sep = sepArg.length() ? sepArg[0] : 0;

  String data;
  if (!collectOperands(args, 0, io, data)) return 0;
  std::vector<String> lines;
  splitLines(data, lines);

  std::vector<std::vector<String>> rows;
  std::vector<unsigned> widths;
  rows.reserve(lines.size());
  for (auto &line : lines) {
    std::vector<String> cells;
    if (sep) {
      int start = 0;
      for (int i = 0; i <= (int)line.length(); ++i) {
        if (i == (int)line.length() || line[i] == sep) {
          cells.push_back(line.substring(start, i));
          start = i + 1;
        }
      }
    } else {
      String cur;
      for (unsigned i = 0; i < line.length(); ++i) {
        if (line[i] == ' ' || line[i] == '\t') {
          if (cur.length()) { cells.push_back(cur); cur = ""; }
        } else {
          cur.concat(line[i]);
        }
      }
      if (cur.length()) cells.push_back(cur);
    }
    for (size_t c = 0; c < cells.size(); ++c) {
      if (widths.size() <= c) widths.push_back(0);
      if (cells[c].length() > widths[c]) widths[c] = cells[c].length();
    }
    rows.push_back(cells);
  }

  for (auto &cells : rows) {
    String out;
    for (size_t c = 0; c < cells.size(); ++c) {
      out.concat(cells[c]);
      if (c + 1 < cells.size())                       // no padding after the last
        for (unsigned p = cells[c].length(); p < widths[c] + 2; ++p) out.concat(' ');
    }
    io.out.println(out);
  }
  return 0;
}

// ---- sponge ----------------------------------------------------------------
// Reads all of stdin before opening the file, so `sort f | sponge f` works
// where `sort f > f` would truncate f before sort ever read it.
static int cmd_sponge(int argc, char **argv, ShellIO &io) {
  ArgScan args(argc, argv);
  if (args.count() < 1) { io.out.println(F("usage: <command> | sponge <file>")); return 1; }
  if (!io.hasIn()) { io.out.println(F("sponge: nothing on stdin")); return 1; }

  const String data = *io.in;        // fully buffered before the open below
  File f = LittleFS.open(resolvePath(args.at(0)), "w");
  if (!f) { io.out.print(args.at(0)); io.out.println(F(": cannot open for writing")); return 1; }
  const String unix = toUnixEol(data);
  f.write((const uint8_t *)unix.c_str(), unix.length());
  f.close();
  return 0;
}

// ---- truncate ---------------------------------------------------------------
static int cmd_truncate(int argc, char **argv, ShellIO &io) {
  ArgScan args(argc, argv, "-s");
  const long size = args.number("-s", -1);
  if (args.count() < 1 || size < 0) {
    io.out.println(F("usage: truncate -s <bytes> <file>"));
    return 1;
  }
  const String name = args.at(0);
  const String abs = resolvePath(name);

  String data;
  if (pathExists(abs) && !readFileToString(abs, data, &io.out, name.c_str())) return 1;

  File f = LittleFS.open(abs, "w");
  if (!f) { io.out.print(name); io.out.println(F(": cannot open for writing")); return 1; }
  if ((long)data.length() >= size) {
    f.write((const uint8_t *)data.c_str(), size);
  } else {
    f.write((const uint8_t *)data.c_str(), data.length());
    for (long i = data.length(); i < size; ++i) f.write((uint8_t)0);   // grow with NULs
  }
  f.close();
  return 0;
}

// ---- readlink ---------------------------------------------------------------
static int cmd_readlink(int argc, char **argv, ShellIO &io) {
  ArgScan args(argc, argv);
  if (args.count() < 1) { io.out.println(F("usage: readlink [-f] <path>")); return 1; }
  // LittleFS has no symlinks, so every path is already its own target. With
  // -f that is exactly realpath; without it, the honest answer is "not a link".
  if (args.letter('f')) { io.out.println(resolvePath(args.at(0))); return 0; }
  io.out.print(args.at(0));
  io.out.println(F(": not a symbolic link (LittleFS has none)"));
  return 1;
}

// ---- bytes : slice input by byte offset --------------------------------------
static int cmd_bytes(int argc, char **argv, ShellIO &io) {
  ArgScan args(argc, argv, "-f -n");
  const long from = args.number("-f", 0);
  const long count = args.number("-n", -1);

  String data;
  if (!collectOperands(args, 0, io, data)) return 0;
  if (from >= (long)data.length() || from < 0) return 0;
  long end = (count < 0) ? (long)data.length() : from + count;
  if (end > (long)data.length()) end = data.length();
  io.out.print(data.substring(from, end));
  return 0;
}

const Command TEXT2_CMDS[] = {
  {"comm",     cmd_comm,     "comm [-123] f1 f2",   "compare two sorted files, 3 columns", G_TEXT},
  {"join",     cmd_join,     "join [-t C] f1 f2",   "join two files on their first field", G_TEXT},
  {"expand",   cmd_expand,   "expand [-t N] [file]","convert tabs to spaces",              G_TEXT},
  {"unexpand", cmd_unexpand, "unexpand [-t N] [f]", "convert leading spaces to tabs",      G_TEXT},
  {"column",   cmd_column,   "column [-s C] [file]","align fields into columns",           G_TEXT},
  {"sponge",   cmd_sponge,   "... | sponge <file>", "soak up stdin, then write the file",  G_TEXT},
  {"truncate", cmd_truncate, "truncate -s N <file>","shrink or extend a file to N bytes",  G_FS},
  {"readlink", cmd_readlink, "readlink [-f] path",  "resolve a path (no links on LittleFS)", G_FS},
  {"bytes",    cmd_bytes,    "bytes [-f N] [-n N]", "slice input by byte offset",          G_TEXT},
};
const size_t TEXT2_CMDS_N = sizeof(TEXT2_CMDS) / sizeof(TEXT2_CMDS[0]);
