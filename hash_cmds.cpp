#include "shell.h"
#include <LittleFS.h>
#include <MD5Builder.h>
#include "mbedtls/sha256.h"

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

const Command HASH_CMDS[] = {
  {"md5sum",    cmd_md5sum,    "md5sum <file>...",    "MD5 digest of files / stdin",     G_TEXT},
  {"sha256sum", cmd_sha256sum, "sha256sum <file>...", "SHA-256 digest of files / stdin", G_TEXT},
  {"crc32",     cmd_crc32,     "crc32 <file>...",     "CRC-32 checksum of files / stdin",G_TEXT},
};
const size_t HASH_CMDS_N = sizeof(HASH_CMDS) / sizeof(HASH_CMDS[0]);
