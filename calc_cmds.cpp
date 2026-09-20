#include "shell.h"
#include <LittleFS.h>
#include <math.h>

// ============================================================================
//  Calculation and data-poking commands.
//
//  `expr` already does one binary operation. This is the rest: a real
//  expression evaluator, a JSON field reader, a checksum, and dd.
// ============================================================================

// ---- bc ---------------------------------------------------------------------
// Recursive descent over the usual precedence ladder. Everything is double;
// the `%` and bitwise operators truncate to long first, as bc does.
//
//   expr    := bitor
//   bitor   := bitxor ('|' bitxor)*
//   bitxor  := bitand ('^' bitand)*          ^ is xor here, ** is power
//   bitand  := shift ('&' shift)*
//   shift   := sum (('<<' | '>>') sum)*
//   sum     := term (('+' | '-') term)*
//   term    := unary (('*' | '/' | '%') unary)*
//   unary   := ('-' | '+' | '~') unary | power
//   power   := atom ('**' unary)?            right-associative
//   atom    := number | '(' expr ')' | name '(' expr ')'

struct Calc {
  const char *p;
  bool ok = true;
  String err;

  void skip() { while (*p == ' ' || *p == '\t') p++; }

  bool eat(const char *tok) {
    skip();
    size_t n = strlen(tok);
    if (strncmp(p, tok, n) != 0) return false;
    // Do not let '<' match the '<<' we were asked for, or '*' match '**'.
    if (n == 1 && (*p == '*' || *p == '<' || *p == '>') && p[1] == *p) return false;
    p += n;
    return true;
  }

  void fail(const char *why) {
    if (ok) { ok = false; err = why; }
  }

  double parse() {
    double v = bitOr();
    skip();
    if (ok && *p) fail("unexpected trailing input");
    return v;
  }

  double bitOr() {
    double v = bitXor();
    for (;;) {
      skip();
      if (*p == '|' && p[1] != '|') { p++; v = (double)((long)v | (long)bitXor()); }
      else return v;
    }
  }
  double bitXor() {
    double v = bitAnd();
    for (;;) {
      skip();
      if (*p == '^') { p++; v = (double)((long)v ^ (long)bitAnd()); }
      else return v;
    }
  }
  double bitAnd() {
    double v = shift();
    for (;;) {
      skip();
      if (*p == '&' && p[1] != '&') { p++; v = (double)((long)v & (long)shift()); }
      else return v;
    }
  }
  double shift() {
    double v = sum();
    for (;;) {
      if (eat("<<"))      v = (double)((long)v << (long)sum());
      else if (eat(">>")) v = (double)((long)v >> (long)sum());
      else return v;
    }
  }
  double sum() {
    double v = term();
    for (;;) {
      skip();
      if (*p == '+')      { p++; v += term(); }
      else if (*p == '-') { p++; v -= term(); }
      else return v;
    }
  }
  double term() {
    double v = unary();
    for (;;) {
      skip();
      if (*p == '*' && p[1] != '*') {
        p++; v *= unary();
      } else if (*p == '/') {
        p++;
        double d = unary();
        if (d == 0) { fail("division by zero"); return 0; }
        v /= d;
      } else if (*p == '%') {
        p++;
        double d = unary();
        if ((long)d == 0) { fail("division by zero"); return 0; }
        v = (double)((long)v % (long)d);
      } else {
        return v;
      }
    }
  }
  double unary() {
    skip();
    if (*p == '-') { p++; return -unary(); }
    if (*p == '+') { p++; return unary(); }
    if (*p == '~') { p++; return (double)(~(long)unary()); }
    return power();
  }
  double power() {
    double base = atom();
    if (eat("**")) return pow(base, unary());   // right-associative
    return base;
  }

  double call(const String &name) {
    double a = bitOr();
    skip();
    if (!eat(")")) { fail("missing ')'"); return 0; }
    if (name == "sqrt")  { if (a < 0) { fail("sqrt of a negative"); return 0; } return sqrt(a); }
    if (name == "abs")   return fabs(a);
    if (name == "int")   return (double)(long)a;
    if (name == "round") return round(a);
    if (name == "floor") return floor(a);
    if (name == "ceil")  return ceil(a);
    if (name == "sin")   return sin(a);
    if (name == "cos")   return cos(a);
    if (name == "tan")   return tan(a);
    if (name == "log")   { if (a <= 0) { fail("log of a non-positive"); return 0; } return log(a); }
    if (name == "log2")  { if (a <= 0) { fail("log of a non-positive"); return 0; } return log(a) / log(2.0); }
    if (name == "log10") { if (a <= 0) { fail("log of a non-positive"); return 0; } return log10(a); }
    if (name == "exp")   return exp(a);
    fail("unknown function");
    return 0;
  }

  double atom() {
    skip();
    if (*p == '(') {
      p++;
      double v = bitOr();
      skip();
      if (!eat(")")) fail("missing ')'");
      return v;
    }
    if (isalpha((int)*p)) {
      String name;
      while (isalnum((int)*p) || *p == '_') name.concat(*p++);
      skip();
      if (*p == '(') { p++; return call(name); }
      if (name == "pi") return M_PI;
      if (name == "e")  return M_E;
      fail("unknown name");
      return 0;
    }
    if (isdigit((int)*p) || *p == '.') {
      char *end = nullptr;
      // 0x / 0b prefixes are handy when poking at registers.
      if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        double v = (double)strtol(p + 2, &end, 16);
        if (end == p + 2) { fail("bad hex literal"); return 0; }
        p = end;
        return v;
      }
      if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        double v = (double)strtol(p + 2, &end, 2);
        if (end == p + 2) { fail("bad binary literal"); return 0; }
        p = end;
        return v;
      }
      double v = strtod(p, &end);
      if (end == p) { fail("bad number"); return 0; }
      p = end;
      return v;
    }
    fail(*p ? "unexpected character" : "unexpected end of expression");
    return 0;
  }
};

static int cmd_bc(int argc, char **argv, ShellIO &io) {
  String src;
  for (int i = 1; i < argc; ++i) { if (src.length()) src.concat(' '); src.concat(argv[i]); }
  if (src.length() == 0 && io.hasIn()) src = *io.in;
  src.trim();
  if (src.length() == 0) {
    io.out.println(F("usage: bc <expression>    eg: bc '(1+2)*sqrt(16)'"));
    io.out.println(F("       + - * / % ** & | ^ ~ << >>,  0x/0b literals,  pi e"));
    io.out.println(F("       sqrt abs int round floor ceil sin cos tan log log2 log10 exp"));
    return 1;
  }

  Calc c{src.c_str()};
  double v = c.parse();
  if (!c.ok) { io.out.print(F("bc: ")); io.out.println(c.err); return 1; }

  // Print integers as integers - "4" beats "4.000000" for the common case.
  if (v == (double)(long long)v && fabs(v) < 1e15) io.out.println((long long)v);
  else io.out.println(String(v, 6));
  return 0;
}

// ---- factor ------------------------------------------------------------------
static int cmd_factor(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: factor <n>...")); return 1; }
  for (int a = 1; a < argc; ++a) {
    long long n = atoll(argv[a]);
    if (n < 2) { io.out.print(argv[a]); io.out.println(F(": not factorable (needs >= 2)")); continue; }
    io.out.print(n);
    io.out.print(':');
    long long rest = n;
    for (long long d = 2; d * d <= rest; d += (d == 2 ? 1 : 2)) {
      while (rest % d == 0) { io.out.print(' '); io.out.print(d); rest /= d; }
    }
    if (rest > 1) { io.out.print(' '); io.out.print(rest); }
    io.out.println();
  }
  return 0;
}

// ---- cksum -------------------------------------------------------------------
// POSIX cksum: CRC-32 over the bytes, then over the length, seeded at zero.
// Deliberately not the same polynomial arrangement as the `crc32` command -
// this one matches coreutils so the numbers can be compared against a PC.
static uint32_t cksumTable(uint8_t i) {
  uint32_t c = (uint32_t)i << 24;
  for (int k = 0; k < 8; ++k)
    c = (c & 0x80000000UL) ? (c << 1) ^ 0x04C11DB7UL : (c << 1);
  return c;
}

static int cmd_cksum(int argc, char **argv, ShellIO &io) {
  String data;
  if (!collectInput(argc, argv, 1, io, data)) return 0;
  uint32_t crc = 0;
  for (unsigned i = 0; i < data.length(); ++i)
    crc = (crc << 8) ^ cksumTable((uint8_t)((crc >> 24) ^ (uint8_t)data[i]));
  for (size_t len = data.length(); len; len >>= 8)
    crc = (crc << 8) ^ cksumTable((uint8_t)((crc >> 24) ^ (len & 0xFF)));
  io.out.print(~crc);
  io.out.print(' ');
  io.out.println(data.length());
  return 0;
}

// ---- jq ----------------------------------------------------------------------
// A field reader, not a JSON engine: `.name`, `.a.b`, and `.arr[2]`. Enough to
// pull a value out of an API response fetched with curl, which is what anyone
// reaches for jq for on a board this size.
static bool jqSkipValue(const String &s, int &i);

static void jqSkipWs(const String &s, int &i) {
  while (i < (int)s.length() && isspace((int)s[i])) i++;
}

static bool jqSkipString(const String &s, int &i) {
  if (i >= (int)s.length() || s[i] != '"') return false;
  i++;
  while (i < (int)s.length()) {
    if (s[i] == '\\') { i += 2; continue; }
    if (s[i] == '"') { i++; return true; }
    i++;
  }
  return false;
}

// Advances `i` past one complete value, whatever its type.
static bool jqSkipValue(const String &s, int &i) {
  jqSkipWs(s, i);
  if (i >= (int)s.length()) return false;
  char c = s[i];
  if (c == '"') return jqSkipString(s, i);
  if (c == '{' || c == '[') {
    char close = (c == '{') ? '}' : ']';
    int depth = 0;
    while (i < (int)s.length()) {
      if (s[i] == '"') { if (!jqSkipString(s, i)) return false; continue; }
      if (s[i] == c) depth++;
      else if (s[i] == close) { depth--; if (depth == 0) { i++; return true; } }
      i++;
    }
    return false;
  }
  while (i < (int)s.length() && s[i] != ',' && s[i] != '}' && s[i] != ']') i++;
  return true;
}

// Finds `key` at the top level of the object starting at `i`.
static bool jqFindKey(const String &s, int &i, const String &key) {
  jqSkipWs(s, i);
  if (i >= (int)s.length() || s[i] != '{') return false;
  i++;
  for (;;) {
    jqSkipWs(s, i);
    if (i >= (int)s.length() || s[i] == '}') return false;
    int keyStart = i;
    if (!jqSkipString(s, i)) return false;
    String found = s.substring(keyStart + 1, i - 1);
    jqSkipWs(s, i);
    if (i >= (int)s.length() || s[i] != ':') return false;
    i++;
    jqSkipWs(s, i);
    if (found == key) return true;
    if (!jqSkipValue(s, i)) return false;
    jqSkipWs(s, i);
    if (i < (int)s.length() && s[i] == ',') i++;
  }
}

static bool jqIndex(const String &s, int &i, int want) {
  jqSkipWs(s, i);
  if (i >= (int)s.length() || s[i] != '[') return false;
  i++;
  for (int n = 0;; ++n) {
    jqSkipWs(s, i);
    if (i >= (int)s.length() || s[i] == ']') return false;
    if (n == want) return true;
    if (!jqSkipValue(s, i)) return false;
    jqSkipWs(s, i);
    if (i < (int)s.length() && s[i] == ',') i++;
  }
}

static int cmd_jq(int argc, char **argv, ShellIO &io) {
  if (argc < 2) {
    io.out.println(F("usage: jq <.path> [file]     eg: curl URL | jq .data.items[0].name"));
    return 1;
  }
  String path = argv[1];
  String data;
  if (!collectInput(argc, argv, 2, io, data)) { io.out.println(F("jq: no input")); return 1; }

  int i = 0;
  jqSkipWs(data, i);

  // Walk the path one .key or [n] step at a time.
  unsigned pi = 0;
  if (pi < path.length() && path[pi] == '.') pi++;
  while (pi < path.length()) {
    if (path[pi] == '[') {
      int close = path.indexOf(']', pi);
      if (close < 0) { io.out.println(F("jq: missing ']'")); return 1; }
      int want = path.substring(pi + 1, close).toInt();
      if (!jqIndex(data, i, want)) { io.out.println(F("jq: index out of range")); return 1; }
      pi = close + 1;
      if (pi < path.length() && path[pi] == '.') pi++;
      continue;
    }
    unsigned start = pi;
    while (pi < path.length() && path[pi] != '.' && path[pi] != '[') pi++;
    String key = path.substring(start, pi);
    if (key.length() == 0) { io.out.println(F("jq: empty key in path")); return 1; }
    if (!jqFindKey(data, i, key)) {
      io.out.print(F("jq: no such key: "));
      io.out.println(key);
      return 1;
    }
    if (pi < path.length() && path[pi] == '.') pi++;
  }

  int end = i;
  if (!jqSkipValue(data, end)) { io.out.println(F("jq: malformed value")); return 1; }
  String value = data.substring(i, end);
  value.trim();
  // Unquote a plain string result; anything else prints as-is.
  if (value.length() >= 2 && value[0] == '"' && value[value.length() - 1] == '"')
    value = value.substring(1, value.length() - 1);
  io.out.println(value);
  return 0;
}

// ---- dd ------------------------------------------------------------------------
static int cmd_dd(int argc, char **argv, ShellIO &io) {
  String inFile, outFile;
  long bs = 512, count = -1, skip = 0, seek = 0;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    int eq = a.indexOf('=');
    if (eq < 0) continue;
    String k = a.substring(0, eq), v = a.substring(eq + 1);
    if      (k == "if")    inFile = v;
    else if (k == "of")    outFile = v;
    else if (k == "bs")    bs = v.toInt();
    else if (k == "count") count = v.toInt();
    else if (k == "skip")  skip = v.toInt();
    else if (k == "seek")  seek = v.toInt();
  }
  if (bs <= 0) { io.out.println(F("dd: bs must be positive")); return 1; }
  if (inFile.length() == 0 && !io.hasIn()) {
    io.out.println(F("usage: dd if=<file> [of=<file>] [bs=N] [count=N] [skip=N] [seek=N]"));
    return 1;
  }

  String data;
  if (inFile.length()) {
    if (!readFileToString(resolvePath(inFile), data, &io.out, inFile.c_str())) return 1;
  } else {
    data = *io.in;
  }

  long start = skip * bs;
  if (start > (long)data.length()) start = data.length();
  long want = (count < 0) ? (long)data.length() - start : count * bs;
  if (start + want > (long)data.length()) want = (long)data.length() - start;
  String chunk = data.substring(start, start + want);

  if (outFile.length() == 0) {
    io.out.print(chunk);
  } else {
    String abs = resolvePath(outFile);
    String existing;
    if (seek > 0 && pathExists(abs)) readFileToString(abs, existing, nullptr);
    File f = LittleFS.open(abs, "w");
    if (!f) { io.out.print(outFile); io.out.println(F(": cannot open for writing")); return 1; }
    long at = seek * bs;
    for (long i = 0; i < at; ++i)
      f.write(i < (long)existing.length() ? (uint8_t)existing[i] : (uint8_t)0);
    f.write((const uint8_t *)chunk.c_str(), chunk.length());
    f.close();
  }
  io.out.printf("%ld bytes (%ld block(s) of %ld)\n", (long)chunk.length(),
                (long)((chunk.length() + bs - 1) / bs), bs);
  return 0;
}

const Command CALC_CMDS[] = {
  {"bc",     cmd_bc,     "bc <expression>",      "evaluate an arithmetic expression", G_SEARCH},
  {"factor", cmd_factor, "factor <n>...",        "prime factors of a number",         G_SEARCH},
  {"cksum",  cmd_cksum,  "cksum [file]",         "POSIX CRC checksum and byte count", G_TEXT},
  {"jq",     cmd_jq,     "jq <.path> [file]",    "read one field out of JSON",        G_TEXT},
  {"dd",     cmd_dd,     "dd if=F [of=F] [bs=N]","copy blocks between files",         G_FS},
};
const size_t CALC_CMDS_N = sizeof(CALC_CMDS) / sizeof(CALC_CMDS[0]);
