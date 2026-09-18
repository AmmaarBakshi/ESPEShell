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

// ---- Serial session ---------------------------------------------------------
static bool serLastCR = false;
static String serLine;

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
//  echoMode: 0 = no echo, 1 = echo char, 2 = echo '*'
//  returns: 0 = nothing yet, 1 = line ready, 2 = line cancelled (Ctrl-C)
// ============================================================================
static int feedChar(uint8_t c, String &line, Print &out, int echoMode, bool &lastCR) {
  if (c == '\r') { lastCR = true; out.print("\r\n"); return 1; }
  if (c == '\n') { if (lastCR) { lastCR = false; return 0; } out.print("\r\n"); return 1; }
  lastCR = false;
  if (c == 0x03) { line = ""; out.print("^C\r\n"); return 2; }          // Ctrl-C
  if (c == 0x08 || c == 0x7f) {                                          // backspace
    if (line.length()) { line.remove(line.length() - 1); if (echoMode == 1) out.print("\b \b"); }
    return 0;
  }
  if (c == '\t') c = ' ';
  if (c >= 0x20 && c < 0x7f) {
    line += (char)c;
    if (echoMode == 1) out.write(c);
    else if (echoMode == 2) out.write('*');
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
      int r = feedChar(b, tnLine, cp, 2, tnLastCR);
      if (r == 1) {
        if (tnLine == g_telnetPassword) {
          cp.println(F("Login OK."));
          tnState = T_SHELL;
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
    int r = feedChar(b, tnLine, cp, 1, tnLastCR);
    if (r == 1) {
      String t = tnLine; t.trim();
      if (t == "exit" || t == "logout") {
        cp.println(F("logout"));
        telnetClient.stop();
        tnState = T_DISCONNECTED;
        g_telnetPeer = "";
        return;
      }
      runLine(tnLine, cp);
      tnLine = "";
      printPrompt(cp);
    } else if (r == 2) {
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
    int r = feedChar(b, serLine, cp, 1, serLastCR);
    if (r == 1) {
      runLine(serLine, cp);
      serLine = "";
      printPrompt(cp);
    } else if (r == 2) {
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
  telnetServer.begin();
  telnetServer.setNoDelay(true);

  CountingPrint cp(Serial);
  printBanner(cp);
  printPrompt(cp);
}

void loop() {
  handleTelnet();
  handleSerial();
  delay(1);
}
