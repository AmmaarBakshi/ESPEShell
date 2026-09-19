// ============================================================================
//  ESPEShell - a Unix-like shell for the ESP32
//
//  Reach it over USB Serial (115200) or over Telnet (port 23) once WiFi is up.
//  Configure WiFi + the login password in config.h before flashing.
// ============================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include "config.h"
#include "shell.h"

// ---- runtime-changeable login password (seed from config, `passwd` updates) --
String g_telnetPassword = TELNET_PASSWORD;

// ---- Telnet server ----------------------------------------------------------
static WiFiServer telnetServer(TELNET_PORT);
static WiFiClient telnetClient;

enum TnState { T_DISCONNECTED, T_AUTH, T_SHELL };
static TnState tnState = T_DISCONNECTED;
static int  tnTries = 0;
static int  tnIac = 0;          // telnet IAC skip state machine
static bool tnLastCR = false;
static String tnLine;

// Serial session state (serSt, a LineEditState) is declared below, next to
// the line editor it belongs to.

// A Print wrapper that counts bytes for the `data` command.
class CountingPrint : public Print {
  Print &t;
 public:
  CountingPrint(Print &tt) : t(tt) {}
  size_t write(uint8_t c) override { g_bytesOut++; return t.write(c); }
  size_t write(const uint8_t *b, size_t n) override { g_bytesOut += n; return t.write(b, n); }
};

// ============================================================================
//  Line editor
//  returns: 0 = nothing yet, 1 = line ready, 2 = line cancelled (Ctrl-C)
// ============================================================================

// Password entry only (Telnet auth): plain char echo as '*', no history/tab -
// getting these wrong for a password field is worse than not having them.
static int feedAuthChar(uint8_t c, String &line, Print &out, bool &lastCR) {
  if (c == '\r') { lastCR = true; out.print("\r\n"); return 1; }
  if (c == '\n') { if (lastCR) { lastCR = false; return 0; } out.print("\r\n"); return 1; }
  lastCR = false;
  if (c == 0x03) { line = ""; out.print("^C\r\n"); return 2; }
  if (c == 0x08 || c == 0x7f) {
    if (line.length()) { line.remove(line.length() - 1); out.print("\b \b"); }
    return 0;
  }
  if (c >= 0x20 && c < 0x7f) { line += (char)c; out.write('*'); }
  return 0;
}

// Per-session state for the rich shell-mode line editor (LineEditState is
// declared in shell.h - see the note there about Arduino auto-prototypes).
static LineEditState serSt;
static LineEditState tnSt;

// Redraws the in-progress line: return to column 0, reprint the prompt,
// erase to end of line, print the (possibly new) line content.
static void redrawLine(Print &out, const String &line) {
  out.print('\r');
  printPrompt(out);
  out.print("\033[K");
  out.print(line);
}

// Shell-mode line editor: backspace, Ctrl-C, Tab completion, Up/Down history.
// Left/Right/Home/End arrow sequences are recognized and swallowed (not
// supported - no mid-line cursor) rather than leaking escape bytes into the
// line buffer.
static int feedShellChar(uint8_t c, LineEditState &st, Print &out) {
  if (st.escState == 1) { st.escState = (c == '[') ? 2 : 0; return 0; }
  if (st.escState == 2) {
    st.escState = 0;
    if (c == 'A' || c == 'B') {  // up / down
      size_t n = historyCount();
      if (n > 0) {
        if (st.histBrowse == -1) st.savedLine = st.line;
        if (c == 'A') { if (st.histBrowse + 1 < (int)n) st.histBrowse++; }
        else          { if (st.histBrowse > -1) st.histBrowse--; }
        st.line = (st.histBrowse == -1) ? st.savedLine : historyGet(st.histBrowse);
        redrawLine(out, st.line);
      }
    }
    return 0;  // left/right/home/end/etc: swallowed, no-op
  }
  if (c == 0x1b) { st.escState = 1; return 0; }  // ESC: start of an arrow-key sequence

  if (c == '\t') {
    String completed = completeLine(st.line, out);
    if (completed != st.line) { st.line = completed; }
    redrawLine(out, st.line);   // also redraws candidate lists back to a clean prompt
    return 0;
  }

  if (c == '\r') { st.lastCR = true; out.print("\r\n"); return 1; }
  if (c == '\n') { if (st.lastCR) { st.lastCR = false; return 0; } out.print("\r\n"); return 1; }
  st.lastCR = false;

  if (c == 0x03) { st.line = ""; st.histBrowse = -1; out.print("^C\r\n"); return 2; }
  if (c == 0x08 || c == 0x7f) {
    if (st.line.length()) { st.line.remove(st.line.length() - 1); out.print("\b \b"); }
    return 0;
  }
  if (c >= 0x20 && c < 0x7f) {
    st.histBrowse = -1;
    st.line += (char)c;
    out.write(c);
  }
  return 0;
}

// WiFi connection is handled by wifiLoadAndConnect() in net_cmds.cpp (reads
// NVS-saved credentials from `wifi set`, falling back to config.h defaults),
// shared with the `wifi` command so there is one connection code path.

// ============================================================================
//  Telnet session
// ============================================================================
static void tnSendOpts() {
  // Ask the client for character-at-a-time mode so line editing works:
  //   IAC WILL ECHO, IAC WILL SUPPRESS-GO-AHEAD
  const uint8_t opts[] = {255, 251, 1, 255, 251, 3};
  telnetClient.write(opts, sizeof(opts));
}

static void tnStartAuth(CountingPrint &cp) {
  if (g_telnetPassword.length() == 0) {
    tnState = T_SHELL;
    printBanner(cp);
    printPrompt(cp);
  } else {
    tnState = T_AUTH;
    tnTries = 0;
    cp.print(F("Password: "));
  }
}

static void handleTelnet() {
  // Accept / reject new connections.
  if (telnetServer.hasClient()) {
    if (telnetClient && telnetClient.connected()) {
      WiFiClient rej = telnetServer.available();
      rej.println(F("ESPEShell: busy (single session only)."));
      rej.stop();
    } else {
      telnetClient = telnetServer.available();
      telnetClient.setNoDelay(true);
      g_telnetPeer = telnetClient.remoteIP().toString();
      tnLine = ""; tnLastCR = false; tnIac = 0;
      tnSt = LineEditState();   // fresh line-editor state for the new connection
      CountingPrint cp(telnetClient);
      tnSendOpts();
      cp.println(F("Connected to ESPEShell."));
      tnStartAuth(cp);
    }
  }

  // Detect disconnect.
  if (telnetClient && !telnetClient.connected()) {
    telnetClient.stop();
    tnState = T_DISCONNECTED;
    g_telnetPeer = "";
    return;
  }
  if (!telnetClient) { tnState = T_DISCONNECTED; return; }

  CountingPrint cp(telnetClient);

  while (telnetClient.available()) {
    uint8_t b = (uint8_t)telnetClient.read();
    g_bytesIn++;

    // Strip Telnet IAC option negotiation.
    if (tnIac == 1) { tnIac = (b >= 251 && b <= 254) ? 2 : 0; continue; }
    if (tnIac == 2) { tnIac = 0; continue; }
    if (b == 255)  { tnIac = 1; continue; }

    if (tnState == T_AUTH) {
      int r = feedAuthChar(b, tnLine, cp, tnLastCR);
      if (r == 1) {
        if (tnLine == g_telnetPassword) {
          cp.println(F("Login OK."));
          tnState = T_SHELL;
          tnSt = LineEditState();   // fresh line-editor state for the shell session
          printBanner(cp);
          printPrompt(cp);
        } else {
          tnTries++;
          if (tnTries >= TELNET_MAX_TRIES) {
            cp.println(F("Too many attempts. Goodbye."));
            telnetClient.stop();
            tnState = T_DISCONNECTED;
            g_telnetPeer = "";
            return;
          }
          cp.print(F("Wrong password.\r\nPassword: "));
        }
        tnLine = "";
      } else if (r == 2) {
        cp.print(F("Password: "));
      }
      continue;
    }

    // T_SHELL
    int r = feedShellChar(b, tnSt, cp);
    if (r == 1) {
      String t = tnSt.line; t.trim();
      if (t == "exit" || t == "logout") {
        cp.println(F("logout"));
        telnetClient.stop();
        tnState = T_DISCONNECTED;
        g_telnetPeer = "";
        return;
      }
      historyAdd(tnSt.line);
      runLine(tnSt.line, cp, &telnetClient);
      tnSt.line = ""; tnSt.histBrowse = -1;
      printPrompt(cp);
    } else if (r == 2) {
      tnSt.histBrowse = -1;
      printPrompt(cp);
    }
  }
}

// ============================================================================
//  Serial session
// ============================================================================
static void handleSerial() {
  CountingPrint cp(Serial);
  while (Serial.available()) {
    uint8_t b = (uint8_t)Serial.read();
    g_bytesIn++;
    int r = feedShellChar(b, serSt, cp);
    if (r == 1) {
      historyAdd(serSt.line);
      runLine(serSt.line, cp, &Serial);
      serSt.line = ""; serSt.histBrowse = -1;
      printPrompt(cp);
    } else if (r == 2) {
      serSt.histBrowse = -1;
      printPrompt(cp);
    }
  }
}

// ============================================================================
//  Arduino entry points
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("[boot] #%lu (see 'dmesg' for the reset reason)\n", (unsigned long)espeBootCount());

  if (!LittleFS.begin(true)) {   // format on first run / mount failure
    Serial.println(F("[fs] LittleFS mount FAILED"));
  } else {
    Serial.printf("[fs] LittleFS mounted (%u KB total)\n",
                  (unsigned)(LittleFS.totalBytes() / 1024));
  }

  wifiLoadAndConnect(Serial);
  if (WiFi.status() == WL_CONNECTED) {
    if (MDNS.begin(g_hostname.c_str())) {
      MDNS.addService("telnet", "tcp", TELNET_PORT);
      Serial.printf("[mdns] connect with  telnet %s.local %d\n", g_hostname.c_str(), TELNET_PORT);
    } else {
      Serial.println(F("[mdns] failed to start (telnet still works via the printed IP)"));
    }
  }
  timeInitAtBoot();   // kicks off a background SNTP sync (no-op without WiFi)

  telnetServer.begin();
  telnetServer.setNoDelay(true);

  CountingPrint cp(Serial);
  printBanner(cp);

  if (LittleFS.exists("/boot.sh")) {
    cp.println(F("[boot] running /boot.sh ..."));
    runLine("sh /boot.sh", cp);
    cp.println(F("[boot] done."));
  }

  printPrompt(cp);
}

// Drains any MQTT messages queued since the last iteration into Serial and
// (if in shell mode) the live Telnet session, then redraws each one's
// in-progress prompt/line - like a real async subscriber, without disturbing
// what's being typed. See mqtt_cmds.cpp.
static void drainMqtt() {
  mqttPoll();
  while (mqttHasPending()) {
    String msg = mqttPopPending();

    CountingPrint scp(Serial);
    scp.println();
    scp.print(F("[mqtt] ")); scp.println(msg);
    redrawLine(scp, serSt.line);

    if (telnetClient && telnetClient.connected() && tnState == T_SHELL) {
      CountingPrint tcp(telnetClient);
      tcp.println();
      tcp.print(F("[mqtt] ")); tcp.println(msg);
      redrawLine(tcp, tnSt.line);
    }
  }
}

void loop() {
  handleTelnet();
  handleSerial();
  drainMqtt();
  delay(1);
}
