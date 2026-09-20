#include "shell.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>

// ============================================================================
//  Networking, part two - the tools for poking at the rest of the LAN.
// ============================================================================

static bool requireNet(ShellIO &io) {
  if (WiFi.status() == WL_CONNECTED) return true;
  io.out.println(F("network is down (WiFi not connected)"));
  return false;
}

static int intFlag(int argc, char **argv, const char *flag, int fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
  return fallback;
}

static const char *strFlag(int argc, char **argv, const char *flag) {
  for (int i = 1; i + 1 < argc; ++i)
    if (strcmp(argv[i], flag) == 0) return argv[i + 1];
  return nullptr;
}

// ---- nc : netcat -------------------------------------------------------------
static int cmd_nc(int argc, char **argv, ShellIO &io) {
  if (argc < 3) {
    io.out.println(F("usage: nc <host> <port> [-w secs] [-q]"));
    io.out.println(F("sends piped stdin, prints what comes back"));
    return 1;
  }
  if (!requireNet(io)) return 1;

  const char *host = argv[1];
  const int port = atoi(argv[2]);
  const int waitSecs = intFlag(argc, argv, "-w", 5);
  bool quiet = false;
  for (int i = 3; i < argc; ++i) if (strcmp(argv[i], "-q") == 0) quiet = true;

  WiFiClient client;
  client.setTimeout(waitSecs);
  if (!client.connect(host, port)) {
    io.out.print(host); io.out.print(':'); io.out.print(port);
    io.out.println(F(": connection refused"));
    return 1;
  }
  if (!quiet) {
    io.out.print(F("connected to ")); io.out.print(host);
    io.out.print(':'); io.out.println(port);
  }

  if (io.hasIn()) {
    // Servers overwhelmingly expect CRLF on the wire; our pipes carry LF.
    String payload = *io.in;
    payload.replace("\n", "\r\n");
    client.print(payload);
  }

  // Read until the peer goes quiet for a full second, or the budget runs out.
  const unsigned long deadline = millis() + (unsigned long)waitSecs * 1000UL;
  unsigned long lastByte = millis();
  size_t total = 0;
  while (client.connected() && millis() < deadline) {
    while (client.available()) {
      io.out.write((uint8_t)client.read());
      total++;
      lastByte = millis();
    }
    if (total && millis() - lastByte > 1000) break;
    if (shellWait(io, 10)) break;          // Ctrl-C
  }
  client.stop();
  if (!quiet) {
    io.out.println();
    io.out.print(F("[")); io.out.print(total); io.out.println(F(" bytes]"));
  }
  return 0;
}

// ---- http : verbs, headers, bodies -------------------------------------------
// curl already does GET. This is the rest of it: POST/PUT/DELETE/HEAD, a custom
// header, a body from an argument or from a pipe, and the response status.
static int cmd_http(int argc, char **argv, ShellIO &io) {
  if (argc < 3) {
    io.out.println(F("usage: http <GET|POST|PUT|DELETE|HEAD|PATCH> <url> [-d body] [-H 'K: V'] [-i]"));
    io.out.println(F("   eg: http POST http://host/api -H 'Content-Type: application/json' -d '{\"a\":1}'"));
    return 1;
  }
  if (!requireNet(io)) return 1;

  String verb = argv[1];
  verb.toUpperCase();
  const String url = argv[2];
  const char *header = strFlag(argc, argv, "-H");
  const char *bodyArg = strFlag(argc, argv, "-d");
  bool showHeaders = false;
  for (int i = 3; i < argc; ++i) if (strcmp(argv[i], "-i") == 0) showHeaders = true;

  String body = bodyArg ? String(bodyArg) : (io.hasIn() ? *io.in : String(""));

  HTTPClient http;
  bool begun = false;
  WiFiClientSecure tls;
  if (url.startsWith("https://")) {
    // No cert bundle on the board, so verification is not possible here. Say
    // so once rather than silently pretending the connection is authenticated.
    tls.setInsecure();
    io.out.println(F("[https: certificate not verified]"));
    begun = http.begin(tls, url);
  } else {
    begun = http.begin(url);
  }
  if (!begun) { io.out.println(F("http: bad URL")); return 1; }

  if (header) {
    String h = header;
    int colon = h.indexOf(':');
    if (colon > 0) {
      String key = h.substring(0, colon);
      String val = h.substring(colon + 1);
      key.trim(); val.trim();
      http.addHeader(key, val);
    }
  }
  if (showHeaders) {
    const char *collect[] = {"Content-Type", "Content-Length", "Server", "Location", "Date"};
    http.collectHeaders(collect, sizeof(collect) / sizeof(collect[0]));
  }

  int code;
  if (body.length()) code = http.sendRequest(verb.c_str(), (uint8_t *)body.c_str(), body.length());
  else               code = http.sendRequest(verb.c_str());

  if (code <= 0) {
    io.out.print(F("http: "));
    io.out.println(HTTPClient::errorToString(code));
    http.end();
    return 1;
  }

  io.out.print(F("HTTP "));
  io.out.println(code);
  if (showHeaders) {
    for (int i = 0; i < http.headers(); ++i) {
      if (http.header(i).length() == 0) continue;
      io.out.print(http.headerName(i));
      io.out.print(F(": "));
      io.out.println(http.header(i));
    }
    io.out.println();
  }
  if (verb != "HEAD") {
    String payload = http.getString();
    io.out.print(payload);
    if (payload.length() && payload[payload.length() - 1] != '\n') io.out.println();
  }
  http.end();
  return (code >= 200 && code < 400) ? 0 : 1;
}

// ---- portscan ------------------------------------------------------------------
// The usual suspects, so a bare `portscan host` is useful without a range.
static const uint16_t COMMON_PORTS[] = {
  21, 22, 23, 25, 53, 80, 110, 143, 443, 445, 554, 587, 631, 993, 995,
  1883, 3000, 3306, 3389, 5000, 5432, 5900, 6379, 8000, 8080, 8443, 8883, 9000,
};

static const char *portName(uint16_t port) {
  switch (port) {
    case 21:   return "ftp";        case 22:   return "ssh";
    case 23:   return "telnet";     case 25:   return "smtp";
    case 53:   return "dns";        case 80:   return "http";
    case 110:  return "pop3";       case 143:  return "imap";
    case 443:  return "https";      case 445:  return "smb";
    case 554:  return "rtsp";       case 631:  return "ipp";
    case 1883: return "mqtt";       case 3306: return "mysql";
    case 3389: return "rdp";        case 5432: return "postgres";
    case 5900: return "vnc";        case 6379: return "redis";
    case 8080: return "http-alt";   case 8883: return "mqtt-tls";
    default:   return "";
  }
}

static int cmd_portscan(int argc, char **argv, ShellIO &io) {
  if (argc < 2) {
    io.out.println(F("usage: portscan <host> [-p from-to] [-t ms]"));
    io.out.println(F("with no -p, scans a list of common ports"));
    return 1;
  }
  if (!requireNet(io)) return 1;

  const char *host = argv[1];
  const int timeoutMs = intFlag(argc, argv, "-t", 300);
  const char *range = strFlag(argc, argv, "-p");

  int from = 0, to = -1;
  if (range) {
    String r = range;
    int dash = r.indexOf('-');
    from = (dash < 0) ? r.toInt() : r.substring(0, dash).toInt();
    to   = (dash < 0) ? from      : r.substring(dash + 1).toInt();
    if (from < 1 || to < from || to > 65535) { io.out.println(F("portscan: bad range")); return 1; }
    if (to - from > 1024) { io.out.println(F("portscan: range too wide (1024 ports max)")); return 1; }
  }

  IPAddress addr;
  if (!WiFi.hostByName(host, addr)) {
    io.out.print(host); io.out.println(F(": cannot resolve"));
    return 1;
  }
  io.out.print(F("scanning ")); io.out.print(addr.toString());
  io.out.println(range ? F(" ...") : F(" (common ports) ..."));

  const int count = range ? (to - from + 1) : (int)(sizeof(COMMON_PORTS) / sizeof(COMMON_PORTS[0]));
  int open = 0;
  for (int i = 0; i < count; ++i) {
    const uint16_t port = range ? (uint16_t)(from + i) : COMMON_PORTS[i];
    WiFiClient probe;
    probe.setTimeout(timeoutMs);
    if (probe.connect(addr, port, timeoutMs)) {
      open++;
      io.out.printf("%6u  open", (unsigned)port);
      const char *name = portName(port);
      if (*name) { io.out.print(F("   ")); io.out.print(name); }
      io.out.println();
      probe.stop();
    }
    if (shellWait(io, 1)) { io.out.println(F("^C")); break; }
  }
  io.out.printf("%d open of %d probed\n", open, count);
  return 0;
}

// ---- netscan : who else is on this /24 ----------------------------------------
static int cmd_netscan(int argc, char **argv, ShellIO &io) {
  if (!requireNet(io)) return 1;
  // A TCP probe to a port nothing listens on still tells us a host is there:
  // a live machine refuses fast, a dead address times out. So the discriminator
  // is latency, and the probe port barely matters.
  const int timeoutMs = intFlag(argc, argv, "-t", 120);
  const int probePort = intFlag(argc, argv, "-p", 80);

  const IPAddress self = WiFi.localIP();
  const IPAddress mask = WiFi.subnetMask();
  if (mask[3] != 0) {
    io.out.println(F("netscan: only a /24 subnet is supported"));
    return 1;
  }
  io.out.printf("scanning %u.%u.%u.1-254 on port %d ...\n", self[0], self[1], self[2], probePort);

  int found = 0;
  for (int host = 1; host <= 254; ++host) {
    IPAddress addr(self[0], self[1], self[2], host);
    WiFiClient probe;
    const unsigned long t0 = millis();
    const bool up = probe.connect(addr, probePort, timeoutMs);
    const unsigned long ms = millis() - t0;
    if (up) {
      probe.stop();
      found++;
      io.out.printf("%-16s open   %lu ms\n", addr.toString().c_str(), ms);
    } else if (ms < (unsigned long)timeoutMs / 2) {
      // Refused rather than timed out: something is at that address.
      found++;
      io.out.printf("%-16s host   %lu ms (refused)\n", addr.toString().c_str(), ms);
    }
    if (shellWait(io, 1)) { io.out.println(F("^C")); break; }
  }
  io.out.printf("%d address(es) responded\n", found);
  return 0;
}

// ---- mdns : browse the local service directory ---------------------------------
static int cmd_mdns(int argc, char **argv, ShellIO &io) {
  if (!requireNet(io)) return 1;
  const char *service = (argc >= 2) ? argv[1] : "http";
  const char *proto   = (argc >= 3) ? argv[2] : "tcp";

  io.out.print(F("browsing _"));
  io.out.print(service); io.out.print(F("._")); io.out.print(proto);
  io.out.println(F(" ..."));

  const int n = MDNS.queryService(service, proto);
  if (n <= 0) { io.out.println(F("no services found")); return 1; }
  for (int i = 0; i < n; ++i) {
    io.out.print(MDNS.hostname(i));
    io.out.print(F("  "));
    io.out.print(MDNS.address(i).toString());
    io.out.print(':');
    io.out.println(MDNS.port(i));
  }
  io.out.printf("%d service(s)\n", n);
  return 0;
}

const Command NET2_CMDS[] = {
  {"nc",       cmd_nc,       "nc <host> <port> [-w s]",    "open a TCP connection (netcat)",  G_NET},
  {"netcat",   cmd_nc,       "netcat <host> <port>",       "open a TCP connection",           G_NET},
  {"http",     cmd_http,     "http <VERB> <url> [-d body]","HTTP request with any verb",      G_NET},
  {"portscan", cmd_portscan, "portscan <host> [-p a-b]",   "probe TCP ports on a host",       G_NET},
  {"netscan",  cmd_netscan,  "netscan [-p port] [-t ms]",  "find live hosts on this /24",     G_NET},
  {"mdns",     cmd_mdns,     "mdns [service] [proto]",     "browse mDNS services on the LAN", G_NET},
};
const size_t NET2_CMDS_N = sizeof(NET2_CMDS) / sizeof(NET2_CMDS[0]);
