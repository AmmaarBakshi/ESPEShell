#include "shell.h"
#include <LittleFS.h>
#include <MD5Builder.h>
#include "mbedtls/sha256.h"
#include "esp_random.h"

// ============================================================================
//  Checksums and digests: md5sum, sha256sum, crc32.
//
//  Each reads its file arguments in chunks (so a digest never needs the whole
//  file in RAM) and falls back to piped stdin when given no file, matching the
//  coreutils habit of `cat x | md5sum`.
// ============================================================================

// Feeds a file (or a string) to a caller-supplied chunk consumer.
typedef void (*ChunkFn)(void *ctx, const uint8_t *data, size_t len);

static bool feedFile(const String &abs, void *ctx, ChunkFn fn, ShellIO &io, const char *name) {
  File f = LittleFS.open(abs, "r");
  if (!f || f.isDirectory()) {
    io.out.print(name);
    io.out.println(F(": cannot read"));
    if (f) f.close();
    return false;
  }
  uint8_t buf[256];
  for (;;) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    fn(ctx, buf, (size_t)n);
  }
  f.close();
  return true;
}

static String hexOf(const uint8_t *d, size_t n) {
  static const char *H = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) { s += H[d[i] >> 4]; s += H[d[i] & 0x0F]; }
  return s;
}

// ---- md5sum ----------------------------------------------------------------
static void md5Chunk(void *ctx, const uint8_t *d, size_t n) {
  ((MD5Builder *)ctx)->add((uint8_t *)d, (uint16_t)n);
}

static String md5Of(ShellIO &io, const String *str, const String &abs, const char *name, bool &ok) {
  MD5Builder b;
  b.begin();
  ok = true;
  if (str) b.add((uint8_t *)str->c_str(), (uint16_t)str->length());
  else ok = feedFile(abs, &b, md5Chunk, io, name);
  b.calculate();
  return b.toString();
}

// ---- sha256sum -------------------------------------------------------------
static void shaChunk(void *ctx, const uint8_t *d, size_t n) {
  mbedtls_sha256_update((mbedtls_sha256_context *)ctx, d, n);
}

static String sha256Of(ShellIO &io, const String *str, const String &abs, const char *name, bool &ok) {
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);   // 0 = SHA-256 (1 would be SHA-224)
  ok = true;
  if (str) mbedtls_sha256_update(&c, (const uint8_t *)str->c_str(), str->length());
  else ok = feedFile(abs, &c, shaChunk, io, name);
  uint8_t out[32];
  mbedtls_sha256_finish(&c, out);
  mbedtls_sha256_free(&c);
  return hexOf(out, sizeof(out));
}

// ---- crc32 -----------------------------------------------------------------
// Standard reflected CRC-32 (the zip/png polynomial), computed bitwise so it
// costs no lookup table in flash.
static void crcChunk(void *ctx, const uint8_t *d, size_t n) {
  uint32_t &crc = *(uint32_t *)ctx;
  for (size_t i = 0; i < n; ++i) {
    crc ^= d[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
  }
}

static String crc32Of(ShellIO &io, const String *str, const String &abs, const char *name, bool &ok) {
  uint32_t crc = 0xFFFFFFFFu;
  ok = true;
  if (str) crcChunk(&crc, (const uint8_t *)str->c_str(), str->length());
  else ok = feedFile(abs, &crc, crcChunk, io, name);
  crc ^= 0xFFFFFFFFu;
  char b[12];
  snprintf(b, sizeof(b), "%08lx", (unsigned long)crc);
  return String(b);
}

// ---- shared driver ---------------------------------------------------------
typedef String (*DigestFn)(ShellIO &, const String *, const String &, const char *, bool &);

static int digestMain(int argc, char **argv, ShellIO &io, DigestFn fn) {
  bool ok = true;
  if (io.hasIn() && argc < 2) {                 // `cat f | md5sum`
    io.out.print(fn(io, io.in, String(), "-", ok));
    io.out.println(F("  -"));
    return 0;
  }
  if (argc < 2) {
    io.out.print(F("usage: "));
    io.out.print(argv[0]);
    io.out.println(F(" <file>...   (or pipe text in)"));
    return 1;
  }
  int rc = 0;
  for (int i = 1; i < argc; ++i) {
    String abs = resolvePath(argv[i]);
    String h = fn(io, nullptr, abs, argv[i], ok);
    if (!ok) { rc = 1; continue; }
    io.out.print(h);
    io.out.print(F("  "));
    io.out.println(argv[i]);
  }
  return rc;
}

static int cmd_md5sum(int argc, char **argv, ShellIO &io)    { return digestMain(argc, argv, io, md5Of); }
static int cmd_sha256sum(int argc, char **argv, ShellIO &io) { return digestMain(argc, argv, io, sha256Of); }
static int cmd_crc32(int argc, char **argv, ShellIO &io)     { return digestMain(argc, argv, io, crc32Of); }

// ---- base64 : encode / decode (text in, text out - pipes welcome) ----------
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static int cmd_base64(int argc, char **argv, ShellIO &io) {
  bool decode = false;
  int first = 1;
  for (; first < argc; ++first) {
    String a = argv[first];
    if (a == "-d" || a == "--decode") decode = true;
    else break;
  }

  String data;
  if (!collectInput(argc, argv, first, io, data)) {
    io.out.println(F("usage: base64 [-d] [file]   (or pipe data in)"));
    return 1;
  }

  if (!decode) {
    size_t len = data.length();
    const uint8_t *d = (const uint8_t *)data.c_str();
    String line;
    for (size_t i = 0; i < len; i += 3) {
      uint32_t v = (uint32_t)d[i] << 16;
      if (i + 1 < len) v |= (uint32_t)d[i + 1] << 8;
      if (i + 2 < len) v |= d[i + 2];
      line += B64[(v >> 18) & 0x3F];
      line += B64[(v >> 12) & 0x3F];
      line += (i + 1 < len) ? B64[(v >> 6) & 0x3F] : '=';
      line += (i + 2 < len) ? B64[v & 0x3F] : '=';
      if (line.length() >= 76) { io.out.println(line); line = ""; }   // wrap like base64(1)
    }
    if (line.length()) io.out.println(line);
    return 0;
  }

  // Decode: ignore whitespace/newlines, stop at padding.
  int acc = 0, bits = 0;
  String out;
  out.reserve(data.length() * 3 / 4 + 3);   // decoded size, less any whitespace
  for (size_t i = 0; i < data.length(); ++i) {
    char c = data[i];
    if (c == '=' ) break;
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    int v = b64Value(c);
    if (v < 0) { io.out.println(F("base64: invalid input")); return 1; }
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out += (char)((acc >> bits) & 0xFF); }
  }
  io.out.print(out);
  if (out.length() && out[out.length() - 1] != '\n') io.out.println();
  return 0;
}

// ---- rand / uuid : the hardware RNG ----------------------------------------
// esp_random() is a true RNG while the radio is on (and a decent PRNG when it
// isn't), so there is no seeding to get wrong here.
static int cmd_rand(int argc, char **argv, ShellIO &io) {
  long lo = 0, hi = 0;
  int count = 1;
  int i = 1;
  if (i < argc && String(argv[i]) == "-n" && i + 1 < argc) { count = String(argv[++i]).toInt(); i++; }
  if (i < argc) {
    long a = String(argv[i]).toInt();
    if (i + 1 < argc) { lo = a; hi = String(argv[i + 1]).toInt(); }
    else { lo = 0; hi = a; }
  } else {
    lo = 0; hi = 0;   // no range: print a raw 32-bit value
  }
  if (count < 1) count = 1;
  if (count > 1000) count = 1000;
  if (hi < lo) { long t = lo; lo = hi; hi = t; }

  for (int k = 0; k < count; ++k) {
    uint32_t r = esp_random();
    if (hi == lo) io.out.println((unsigned long)r);
    else io.out.println((long)(lo + (long)(r % (uint32_t)(hi - lo + 1))));
  }
  return 0;
}

static int cmd_uuid(int argc, char **argv, ShellIO &io) {
  int count = (argc >= 2) ? String(argv[1]).toInt() : 1;
  if (count < 1) count = 1;
  if (count > 100) count = 100;
  for (int k = 0; k < count; ++k) {
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
      uint32_t r = esp_random();
      b[i] = r >> 24; b[i + 1] = r >> 16; b[i + 2] = r >> 8; b[i + 3] = r;
    }
    b[6] = (b[6] & 0x0F) | 0x40;   // version 4
    b[8] = (b[8] & 0x3F) | 0x80;   // variant 1
    String h = hexOf(b, 16);
    io.out.println(h.substring(0, 8) + "-" + h.substring(8, 12) + "-" + h.substring(12, 16) +
                   "-" + h.substring(16, 20) + "-" + h.substring(20));
  }
  return 0;
}

const Command HASH_CMDS[] = {
  {"rand",      cmd_rand,      "rand [-n N] [lo] hi", "random numbers (hardware RNG)",   G_TEXT},
  {"uuid",      cmd_uuid,      "uuid [count]",        "random v4 UUIDs",                 G_TEXT},
  {"base64",    cmd_base64,    "base64 [-d] [file]",  "base64 encode / decode",          G_TEXT},
  {"md5sum",    cmd_md5sum,    "md5sum <file>...",    "MD5 digest of files / stdin",     G_TEXT},
  {"sha256sum", cmd_sha256sum, "sha256sum <file>...", "SHA-256 digest of files / stdin", G_TEXT},
  {"crc32",     cmd_crc32,     "crc32 <file>...",     "CRC-32 checksum of files / stdin",G_TEXT},
};
const size_t HASH_CMDS_N = sizeof(HASH_CMDS) / sizeof(HASH_CMDS[0]);
