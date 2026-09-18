#include "shell.h"
#include <LittleFS.h>

// ============================================================================
//  File transfer: send / recv
//
//  ssh/scp/sftp aren't feasible on a stock ESP32 (see net_cmds.cpp), so real
//  file transfer here is base64-over-the-existing-connection: plain ASCII
//  text that works over a raw Telnet/Serial line shell with zero extra tools
//  (no client software, no separate binary mode) - at the cost of ~33% size
//  overhead versus a true binary protocol like XMODEM.
// ============================================================================

static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String base64Encode(const uint8_t *data, size_t len) {
  String out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
    out += B64_ALPHABET[(v >> 18) & 0x3F];
    out += B64_ALPHABET[(v >> 12) & 0x3F];
    out += B64_ALPHABET[(v >> 6) & 0x3F];
    out += B64_ALPHABET[v & 0x3F];
    i += 3;
  }
  size_t rem = len - i;
  if (rem == 1) {
    uint32_t v = (uint32_t)data[i] << 16;
    out += B64_ALPHABET[(v >> 18) & 0x3F];
    out += B64_ALPHABET[(v >> 12) & 0x3F];
    out += "==";
  } else if (rem == 2) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
    out += B64_ALPHABET[(v >> 18) & 0x3F];
    out += B64_ALPHABET[(v >> 12) & 0x3F];
    out += B64_ALPHABET[(v >> 6) & 0x3F];
    out += '=';
  }
  return out;
}

static int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static bool looksBase64(const String &s) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (b64Val(c) < 0 && c != '=') return false;
  }
  return true;
}

// Decodes one self-contained base64 line (as emitted by `send`, one line per
// 57 input bytes -> a multiple of 4 base64 chars) and appends the bytes to f.
static size_t base64DecodeAppend(const String &line, File &f) {
  int vals[4], n = 0;
  size_t written = 0;
  for (size_t i = 0; i < line.length(); ++i) {
    char c = line[i];
    if (c == '=') break;               // padding: nothing meaningful follows
    int v = b64Val(c);
    if (v < 0) continue;               // ignore stray whitespace/CR
    vals[n++] = v;
    if (n < 4) continue;
    uint8_t triple[3] = {
        (uint8_t)((vals[0] << 2) | (vals[1] >> 4)),
        (uint8_t)((vals[1] << 4) | (vals[2] >> 2)),
        (uint8_t)((vals[2] << 6) | vals[3]),
    };
    f.write(triple, 3);
    written += 3;
    n = 0;
  }
  if (n == 2) {
    uint8_t b0 = (vals[0] << 2) | (vals[1] >> 4);
    f.write(&b0, 1);
    written += 1;
  } else if (n == 3) {
    uint8_t pair[2] = {
        (uint8_t)((vals[0] << 2) | (vals[1] >> 4)),
        (uint8_t)((vals[1] << 4) | (vals[2] >> 2)),
    };
    f.write(pair, 2);
    written += 2;
  }
  return written;
}

// ---- send : print a file as base64 -----------------------------------------
static int cmd_send(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: send <file>   (prints base64 - paste it into a 'recv' session)")); return 1; }
  String abs = resolvePath(argv[1]);
  File f = LittleFS.open(abs, "r");
  if (!f || f.isDirectory()) { io.out.print(argv[1]); io.out.println(F(": cannot read")); if (f) f.close(); return 1; }

  io.out.print(F("----BEGIN ")); io.out.print(argv[1]); io.out.println(F(" (base64)----"));
  uint8_t buf[57];  // 57 bytes -> exactly 76 base64 chars, a classic MIME-style line
  size_t total = 0;
  while (true) {
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    io.out.println(base64Encode(buf, n));
    total += n;
  }
  f.close();
  io.out.println(F("----END----"));
  io.out.println('.');   // sentinel: lets a `recv` on the other end auto-stop
  io.out.printf("(%u bytes)\n", (unsigned)total);
  return 0;
}

// ---- recv : paste base64 text back into a file -----------------------------
static int cmd_recv(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: recv <file>   (paste base64 - e.g. from 'send' - end with a line containing only .)")); return 1; }
  if (!io.rawIn) { io.out.println(F("recv: no interactive connection to read from")); return 1; }

  String abs = resolvePath(argv[1]);
  File f = LittleFS.open(abs, "w");
  if (!f) { io.out.println(F("recv: cannot open file for writing")); return 1; }

  io.out.println(F("Paste base64 data now (BEGIN/END framing is ignored). End with a line containing only: ."));
  String line;
  unsigned long lastByte = millis();
  size_t total = 0;
  const unsigned long IDLE_TIMEOUT_MS = 30000;

  while (true) {
    if (io.rawIn->available()) {
      char c = (char)io.rawIn->read();
      g_bytesIn++;
      lastByte = millis();
      if (c == '\r') continue;
      if (c == '\n') {
        if (line == ".") break;
        if (looksBase64(line)) total += base64DecodeAppend(line, f);
        // anything else (BEGIN/END markers, blank lines) is silently skipped
        line = "";
      } else {
        line += c;
      }
    } else if (millis() - lastByte > IDLE_TIMEOUT_MS) {
      io.out.println(F("\nrecv: timed out waiting for data"));
      f.close();
      return 1;
    } else {
      delay(2);
    }
  }
  f.close();
  io.out.printf("recv: wrote %u bytes to %s\n", (unsigned)total, argv[1]);
  return 0;
}

const Command XFER_CMDS[] = {
  {"send", cmd_send, "send <file>", "print a file as base64 (paste into recv)", G_XFER},
  {"recv", cmd_recv, "recv <file>", "receive base64-pasted data into a file",   G_XFER},
};
const size_t XFER_CMDS_N = sizeof(XFER_CMDS) / sizeof(XFER_CMDS[0]);
