#include "shell.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_mac.h>

// ============================================================================
//  Networking commands + environment/echo/printf built-ins
// ============================================================================

static bool netUp(ShellIO &io) {
  if (WiFi.status() == WL_CONNECTED) return true;
  io.out.println(F("network is down (WiFi not connected)"));
  return false;
}

// ---- WiFi connect (used at boot and by the `wifi` command) ----------------
bool wifiConnect(const String &ssid, const String &pass, unsigned long timeoutMs, Print &out) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(g_hostname.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(300);
    out.print('.');
  }
  return WiFi.status() == WL_CONNECTED;
}

// Reads saved credentials from NVS (set via `wifi set`); falls back to the
// compiled-in config.h defaults if none have been saved yet. Called once
// from setup().
void wifiLoadAndConnect(Print &out) {
  String ssid, pass;
  bool haveSaved = false;
  Preferences prefs;
  if (prefs.begin(ESPE_PREFS_NAMESPACE, true)) {  // read-only
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");
    prefs.end();
    haveSaved = ssid.length() > 0;
  }
  if (!haveSaved) { ssid = WIFI_SSID; pass = WIFI_PASSWORD; }

  out.printf("[wifi] connecting to \"%s\" (%s) ", ssid.c_str(), haveSaved ? "saved" : "config.h");
  bool ok = wifiConnect(ssid, pass, WIFI_TIMEOUT_MS, out);
  out.println();
  if (ok) {
    out.print(F("[wifi] connected, IP: "));
    out.println(WiFi.localIP());
    out.printf("[wifi] telnet: connect with  telnet %s %d\n", WiFi.localIP().toString().c_str(), TELNET_PORT);
  } else {
    out.println(F("[wifi] not connected - running on Serial only (try 'wifi set <ssid> <pass>' once connected some other way, or fix config.h and reflash)"));
  }
}

// ---- wifi : runtime WiFi configuration, persisted in NVS -------------------
static int cmd_wifi(int argc, char **argv, ShellIO &io) {
  String sub = argc >= 2 ? String(argv[1]) : String("status");

  if (sub == "status") {
    Preferences prefs;
    bool haveSaved = false;
    if (prefs.begin(ESPE_PREFS_NAMESPACE, true)) { haveSaved = prefs.getString("ssid", "").length() > 0; prefs.end(); }
    io.out.print(F("credentials : ")); io.out.println(haveSaved ? F("saved in NVS (from 'wifi set')") : F("config.h default"));
    io.out.print(F("state       : "));
    if (WiFi.status() == WL_CONNECTED) {
      io.out.print(F("connected to \"")); io.out.print(WiFi.SSID()); io.out.println('"');
      io.out.print(F("  ip   : ")); io.out.println(WiFi.localIP());
      io.out.print(F("  rssi : ")); io.out.print(WiFi.RSSI()); io.out.println(F(" dBm"));
    } else {
      io.out.println(F("disconnected"));
    }
    return 0;
  }

  if (sub == "set") {
    if (argc < 4) { io.out.println(F("usage: wifi set <ssid> <password>")); return 1; }
    String ssid = argv[2], pass = argv[3];
    Preferences prefs;
    if (!prefs.begin(ESPE_PREFS_NAMESPACE, false)) { io.out.println(F("wifi: failed to open NVS storage")); return 1; }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    io.out.println(F("saved to NVS. connecting now..."));
    WiFi.disconnect();
    bool ok = wifiConnect(ssid, pass, WIFI_TIMEOUT_MS, io.out);
    io.out.println();
    if (ok) { io.out.print(F("connected, ip=")); io.out.println(WiFi.localIP()); }
    else io.out.println(F("could not connect with the new credentials (still saved - will retry on next boot)"));
    return ok ? 0 : 1;
  }

  if (sub == "forget") {
    Preferences prefs;
    if (prefs.begin(ESPE_PREFS_NAMESPACE, false)) { prefs.clear(); prefs.end(); }
    io.out.println(F("saved WiFi credentials cleared - config.h defaults will be used on next boot"));
    return 0;
  }

  io.out.println(F("usage: wifi status | wifi set <ssid> <pass> | wifi forget"));
  return 1;
}


static int maskBits(IPAddress m) {
  uint32_t v = (uint32_t)m;
  int bits = 0;
  for (int i = 0; i < 32; ++i) if (v & (1u << i)) bits++;
  return bits;
}

// ---- ip --------------------------------------------------------------------
static int cmd_ip(int argc, char **argv, ShellIO &io) {
  String sub = argc >= 2 ? String(argv[1]) : String("addr");
  bool up = (WiFi.status() == WL_CONNECTED);

  if (sub == "link") {
    io.out.printf("1: wlan0: <%s> mtu 1500\n", up ? "UP,RUNNING" : "DOWN");
    io.out.print(F("    ether ")); io.out.println(WiFi.macAddress());
    return 0;
  }
  if (sub == "route") {
    if (up) { io.out.print(F("default via ")); io.out.print(WiFi.gatewayIP()); io.out.println(F(" dev wlan0")); }
    else io.out.println(F("(no route: WiFi down)"));
    return 0;
  }
  // addr (default)
  io.out.printf("1: wlan0: <%s> mtu 1500\n", up ? "UP,RUNNING" : "DOWN");
  if (up) {
    io.out.print(F("    inet ")); io.out.print(WiFi.localIP());
    io.out.print('/'); io.out.print(maskBits(WiFi.subnetMask()));
    io.out.println();
    io.out.print(F("    ether ")); io.out.println(WiFi.macAddress());
    io.out.print(F("    ssid ")); io.out.print(WiFi.SSID());
    io.out.print(F("  rssi ")); io.out.print(WiFi.RSSI()); io.out.print(F(" dBm  ch "));
    io.out.println(WiFi.channel());
    io.out.print(F("    gateway ")); io.out.print(WiFi.gatewayIP());
    io.out.print(F("  dns ")); io.out.println(WiFi.dnsIP());
  } else {
    io.out.print(F("    ether ")); io.out.println(WiFi.macAddress());
    io.out.println(F("    (not associated)"));
  }
  return 0;
}

// ---- ping (TCP connect check) ----------------------------------------------
static int cmd_ping(int argc, char **argv, ShellIO &io) {
  int count = 4;
  uint16_t port = 80;
  String host;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-c" && i + 1 < argc) count = atoi(argv[++i]);
    else if (a == "-p" && i + 1 < argc) port = atoi(argv[++i]);
    else if (a[0] != '-') host = a;
  }
  if (host.length() == 0) { io.out.println(F("usage: ping [-c N] [-p port] host")); return 1; }
  if (!netUp(io)) return 1;
  IPAddress ip;
  if (!WiFi.hostByName(host.c_str(), ip)) { io.out.print(F("ping: cannot resolve ")); io.out.println(host); return 2; }
  io.out.printf("PING %s (%s) tcp/%u:\n", host.c_str(), ip.toString().c_str(), port);
  int ok = 0;
  unsigned long tmin = 999999, tmax = 0, tsum = 0;
  for (int i = 0; i < count; ++i) {
    WiFiClient c;
    unsigned long t0 = millis();
    bool r = c.connect(ip, port, 2000);
    unsigned long dt = millis() - t0;
    c.stop();
    if (r) {
      ok++; tsum += dt;
      if (dt < tmin) tmin = dt;
      if (dt > tmax) tmax = dt;
      io.out.printf("  reply from %s: seq=%d time=%lu ms\n", ip.toString().c_str(), i + 1, dt);
    } else {
      io.out.printf("  seq=%d: no response (timeout)\n", i + 1);
    }
    if (i + 1 < count) delay(300);
  }
  io.out.printf("--- %s statistics ---\n", host.c_str());
  io.out.printf("%d sent, %d received, %d%% loss", count, ok, count ? (count - ok) * 100 / count : 0);
  if (ok) io.out.printf(", rtt min/avg/max = %lu/%lu/%lu ms", tmin, tsum / ok, tmax);
  io.out.println();
  return ok ? 0 : 1;
}

// ---- HTTP helper -----------------------------------------------------------
static bool httpGet(const String &url, String &body, int &code, ShellIO &io) {
  HTTPClient http;
  bool ok;
  if (url.startsWith("https")) {
    WiFiClientSecure *sc = new WiFiClientSecure();
    sc->setInsecure();
    ok = http.begin(*sc, url);
    if (ok) { code = http.GET(); if (code > 0) body = http.getString(); }
    http.end();
    delete sc;
  } else {
    WiFiClient c;
    ok = http.begin(c, url);
    if (ok) { code = http.GET(); if (code > 0) body = http.getString(); }
    http.end();
  }
  return ok && code > 0;
}

// ---- curl ------------------------------------------------------------------
static int cmd_curl(int argc, char **argv, ShellIO &io) {
  String url, outFile;
  bool silent = false, headOnly = false;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-s") silent = true;
    else if (a == "-I") headOnly = true;
    else if (a == "-o" && i + 1 < argc) outFile = argv[++i];
    else if (a[0] != '-') url = a;
  }
  if (url.length() == 0) { io.out.println(F("usage: curl [-s] [-o file] URL")); return 1; }
  if (!netUp(io)) return 1;
  String body; int code = 0;
  if (!httpGet(url, body, code, io)) { io.out.printf("curl: request failed (code %d)\n", code); return 1; }
  if (!silent && (code < 200 || code >= 300)) io.out.printf("curl: HTTP %d\n", code);
  if (headOnly) { io.out.printf("HTTP %d, %u bytes\n", code, (unsigned)body.length()); return 0; }
  if (outFile.length()) {
    File f = LittleFS.open(resolvePath(outFile), "w");
    if (!f) { io.out.println(F("curl: cannot open output file")); return 1; }
    f.print(body); f.close();
    io.out.printf("saved %u bytes to %s\n", (unsigned)body.length(), outFile.c_str());
  } else {
    io.out.print(body);
    if (body.length() && !body.endsWith("\n")) io.out.println();
  }
  return 0;
}

// ---- wget ------------------------------------------------------------------
static int cmd_wget(int argc, char **argv, ShellIO &io) {
  String url, outFile;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if ((a == "-O" || a == "-o") && i + 1 < argc) outFile = argv[++i];
    else if (a[0] != '-') url = a;
  }
  if (url.length() == 0) { io.out.println(F("usage: wget [-O file] URL")); return 1; }
  if (!netUp(io)) return 1;
  if (outFile.length() == 0) {
    int sl = url.lastIndexOf('/');
    outFile = (sl >= 0 && sl + 1 < (int)url.length()) ? url.substring(sl + 1) : String("index.html");
    int q = outFile.indexOf('?'); if (q >= 0) outFile = outFile.substring(0, q);
    if (outFile.length() == 0) outFile = "index.html";
  }
  String body; int code = 0;
  if (!httpGet(url, body, code, io)) { io.out.printf("wget: failed (code %d)\n", code); return 1; }
  File f = LittleFS.open(resolvePath(outFile), "w");
  if (!f) { io.out.println(F("wget: cannot open output file")); return 1; }
  f.print(body); f.close();
  io.out.printf("'%s' saved [%u bytes] (HTTP %d)\n", outFile.c_str(), (unsigned)body.length(), code);
  return 0;
}

// ---- dig / nslookup --------------------------------------------------------
static int cmd_dig(int argc, char **argv, ShellIO &io) {
  if (argc < 2) { io.out.println(F("usage: dig host")); return 1; }
  if (!netUp(io)) return 1;
  IPAddress ip;
  if (!WiFi.hostByName(argv[1], ip)) { io.out.print(F(";; cannot resolve ")); io.out.println(argv[1]); return 1; }
  io.out.print(argv[1]); io.out.print(F(".  IN  A  ")); io.out.println(ip);
  return 0;
}

// ---- ss --------------------------------------------------------------------
static int cmd_ss(int argc, char **argv, ShellIO &io) {
  io.out.println(F("Netid  State      Local Address:Port     Peer Address:Port"));
  if (WiFi.status() == WL_CONNECTED) {
    io.out.printf("tcp    LISTEN     %s:%d          *:*\n", WiFi.localIP().toString().c_str(), TELNET_PORT);
    if (g_telnetPeer.length())
      io.out.printf("tcp    ESTAB      %s:%d          %s\n", WiFi.localIP().toString().c_str(), TELNET_PORT, g_telnetPeer.c_str());
  } else {
    io.out.println(F("(WiFi down - telnet listener unreachable)"));
  }
  return 0;
}

// ---- stubs (ssh/scp/sftp/traceroute) ---------------------------------------
static int cmd_ssh_stub(int argc, char **argv, ShellIO &io) {
  io.out.print(argv[0]);
  io.out.println(F(": ESPEShell IS the remote shell - connect TO this device with:"));
  io.out.print(F("      telnet "));
  io.out.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("<device-ip>"));
  return 1;
}
static int cmd_traceroute(int argc, char **argv, ShellIO &io) {
  io.out.println(F("traceroute: not supported (no raw ICMP). Try ping / dig."));
  return 1;
}

// ---- env / export / echo / printf ------------------------------------------
static int cmd_env(int argc, char **argv, ShellIO &io) {
  io.out.print(F("PWD=")); io.out.println(g_cwd);
  io.out.print(F("USER=")); io.out.println(g_user);
  io.out.print(F("HOSTNAME=")); io.out.println(g_hostname);
  io.out.println(F("HOME=/"));
  io.out.println(F("SHELL=/espeshell"));
  for (auto &kv : envAll()) { io.out.print(kv.first); io.out.print('='); io.out.println(kv.second); }
  return 0;
}

static int cmd_export(int argc, char **argv, ShellIO &io) {
  if (argc < 2) return cmd_env(argc, argv, io);
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    int eq = a.indexOf('=');
    if (eq >= 0) envSet(a.substring(0, eq), a.substring(eq + 1));
    else if (envGet(a).length() == 0) envSet(a, "");
  }
  return 0;
}

static String interpEscapes(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] == '\\' && i + 1 < s.length()) {
      char n = s[++i];
      switch (n) { case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break;
                   case '\\': o += '\\'; break; case 'e': o += '\033'; break; case '0': o += '\0'; break;
                   default: o += '\\'; o += n; }
    } else o += s[i];
  }
  return o;
}

static int cmd_echo(int argc, char **argv, ShellIO &io) {
  bool noNL = false, esc = false;
  int start = 1;
  while (start < argc) {
    String a = argv[start];
    if (a == "-n") { noNL = true; start++; }
    else if (a == "-e") { esc = true; start++; }
    else if (a == "-ne" || a == "-en") { noNL = esc = true; start++; }
    else break;
  }
  String out;
  for (int i = start; i < argc; ++i) { if (i > start) out += ' '; out += expandVars(argv[i]); }
  if (esc) out = interpEscapes(out);
  io.out.print(out);
  if (!noNL) io.out.println();
  return 0;
}

static int cmd_printf(int argc, char **argv, ShellIO &io) {
  if (argc < 2) return 0;
  String fmt = interpEscapes(expandVars(argv[1]));
  int ai = 2;
  for (size_t i = 0; i < fmt.length(); ++i) {
    char c = fmt[i];
    if (c != '%') { io.out.print(c); continue; }
    if (i + 1 >= fmt.length()) { io.out.print('%'); break; }
    char conv = fmt[++i];
    String arg = (ai < argc) ? String(argv[ai]) : String("");
    switch (conv) {
      case '%': io.out.print('%'); break;
      case 's': io.out.print(arg); ai++; break;
      case 'd': case 'i': io.out.print((long)arg.toInt()); ai++; break;
      case 'x': io.out.print((unsigned long)arg.toInt(), HEX); ai++; break;
      case 'c': io.out.print(arg.length() ? arg[0] : ' '); ai++; break;
      default: io.out.print('%'); io.out.print(conv);
    }
  }
  return 0;
}

// ---- ap : run the board's own access point ---------------------------------
// Useful away from a known network: the ESP32 becomes the network, and the
// Telnet shell (plus `httpd`) is reachable at the AP's own address. Started
// alongside the station interface (WIFI_AP_STA) so an existing WiFi
// connection - and this very session - survives turning the AP on.
static int cmd_ap(int argc, char **argv, ShellIO &io) {
  String sub = (argc >= 2) ? String(argv[1]) : String("status");

  if (sub == "start") {
    String ssid = (argc >= 3) ? String(argv[2]) : (String("ESPEShell-") + g_hostname);
    String pass = (argc >= 4) ? String(argv[3]) : String("");
    if (pass.length() > 0 && pass.length() < 8) {
      io.out.println(F("ap: WPA2 needs a password of at least 8 characters (or none for open)"));
      return 1;
    }
    WiFi.mode(WIFI_AP_STA);
    bool ok = pass.length() ? WiFi.softAP(ssid.c_str(), pass.c_str())
                            : WiFi.softAP(ssid.c_str());
    if (!ok) { io.out.println(F("ap: failed to start")); return 1; }
    io.out.print(F("ap: \""));
    io.out.print(ssid);
    io.out.print(F("\" up at "));
    io.out.print(WiFi.softAPIP().toString());
    io.out.println(pass.length() ? F("  (WPA2)") : F("  (open network)"));
    io.out.printf("    telnet %s %d\n", WiFi.softAPIP().toString().c_str(), TELNET_PORT);
    return 0;
  }

  if (sub == "stop") {
    if (WiFi.softAPgetStationNum() > 0)
      io.out.printf("ap: dropping %u connected client(s)\n", (unsigned)WiFi.softAPgetStationNum());
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    io.out.println(F("ap: stopped"));
    return 0;
  }

  if (sub == "status") {
    String apSsid = WiFi.softAPSSID();
    if (apSsid.length() == 0) { io.out.println(F("ap: not running  (ap start [ssid] [pass])")); return 0; }
    io.out.print(F("ssid:     ")); io.out.println(apSsid);
    io.out.print(F("address:  ")); io.out.println(WiFi.softAPIP().toString());
    io.out.print(F("mac:      ")); io.out.println(WiFi.softAPmacAddress());
    io.out.printf("clients:  %u\n", (unsigned)WiFi.softAPgetStationNum());
    return 0;
  }

  io.out.println(F("usage: ap start [ssid] [password] | stop | status"));
  return 1;
}

// ---- mac -------------------------------------------------------------------
// The four interface MACs are all derived from the one base address burned
// into eFuse, so `mac -b` is the number that actually identifies the chip.
static void macLine(ShellIO &io, const char *label, esp_mac_type_t t) {
  uint8_t m[6] = {0};
  if (esp_read_mac(m, t) != ESP_OK) return;
  io.out.printf("%-9s %02X:%02X:%02X:%02X:%02X:%02X
", label, m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int cmd_mac(int argc, char **argv, ShellIO &io) {
  bool base = false;
  for (int i = 1; i < argc; ++i) {
    String a = argv[i];
    if (a == "-b" || a == "--base") base = true;
    else { io.out.print(a); io.out.println(F(": unknown option")); return 1; }
  }
  uint8_t m[6] = {0};
  esp_efuse_mac_get_default(m);
  if (base) {
    io.out.printf("%02X:%02X:%02X:%02X:%02X:%02X
", m[0], m[1], m[2], m[3], m[4], m[5]);
    return 0;
  }
  io.out.printf("%-9s %02X:%02X:%02X:%02X:%02X:%02X
", "base", m[0], m[1], m[2], m[3], m[4], m[5]);
  macLine(io, "wifi-sta", ESP_MAC_WIFI_STA);
  macLine(io, "wifi-ap", ESP_MAC_WIFI_SOFTAP);
  macLine(io, "bt", ESP_MAC_BT);
  macLine(io, "ethernet", ESP_MAC_ETH);
  return 0;
}

const Command NET_CMDS[] = {
  {"ap",         cmd_ap,         "ap start [ssid] [pass]|stop", "run the board's own access point", G_NET},
  {"mac",        cmd_mac,        "mac [-b]",             "show the interface MAC addresses", G_NET},
  {"ip",         cmd_ip,         "ip [addr|link|route]", "show network configuration",   G_NET},
  {"wifi",       cmd_wifi,       "wifi status|set <s> <p>|forget", "configure WiFi (persists in NVS)", G_NET},
  {"ping",       cmd_ping,       "ping [-c N] host",     "TCP reachability check",        G_NET},
  {"curl",       cmd_curl,       "curl [-o f] URL",      "HTTP(S) GET to stdout/file",    G_NET},
  {"wget",       cmd_wget,       "wget [-O f] URL",      "download a URL to a file",      G_NET},
  {"dig",        cmd_dig,        "dig host",             "DNS lookup",                    G_NET},
  {"nslookup",   cmd_dig,        "nslookup host",        "DNS lookup",                    G_NET},
  {"ss",         cmd_ss,         "ss",                   "socket / listener status",      G_NET},
  {"traceroute", cmd_traceroute, "traceroute host",      "(unsupported: no raw ICMP)",    G_NET},
  {"ssh",        cmd_ssh_stub,   "ssh ...",              "(this device is the server)",   G_NET},
  {"scp",        cmd_ssh_stub,   "scp ...",              "(use curl/wget instead)",       G_NET},
  {"sftp",       cmd_ssh_stub,   "sftp ...",             "(use curl/wget instead)",       G_NET},
  {"env",        cmd_env,        "env",                  "print environment variables",   G_ENV},
  {"export",     cmd_export,     "export NAME=VAL",      "set an environment variable",   G_ENV},
  {"echo",       cmd_echo,       "echo [-ne] text",      "print text (expands $VARS)",    G_ENV},
  {"printf",     cmd_printf,     "printf FMT [args]",    "formatted print",               G_ENV},
};
const size_t NET_CMDS_N = sizeof(NET_CMDS) / sizeof(NET_CMDS[0]);
