#include "shell.h"
#include "config.h"
#include <WiFi.h>

// ============================================================================
//  Host bridge - talk to the laptop that ESPEShell is plugged into
//
//  The ESP32 cannot see the machine on the other end of the wire by itself, so
//  a small agent runs there (tools/espehost.py) and answers questions about its
//  hardware: battery and current draw, cameras, USB/HID/audio devices, CPU,
//  memory, disks, network, processes.
//
//  The laptop dials *out* to us, not the other way round. Our address is the
//  stable one (mDNS, or the IP printed at boot); a laptop's is not, and its
//  inbound ports are usually firewalled. So the ESP32 listens and the agent
//  connects, then the link is symmetric.
//
//  Wire format: one JSON object per line, in both directions.
//
//    ESP32 -> agent   {"id":7,"op":"power","args":"--watts"}
//    agent -> ESP32   {"id":7,"ok":true,"text":"...","val":12.5}
//                     {"id":7,"ok":false,"err":"no battery"}
//    agent -> ESP32   {"ev":"cam","text":"motion on /dev/video0"}   (unsolicited)
//
//  Replies carry text the agent has already formatted. That is deliberate: the
//  laptop has Python and a screen's worth of width, the ESP32 has neither, and
//  it keeps a JSON parser out of the firmware. All this file needs is a scalar
//  field reader, which is what jsonField()/jsonNumber() below are.
//
//  `val` is the machine-readable half, used by fusion rules (see fuse_cmds.cpp)
//  so a threshold can be compared without parsing prose.
// ============================================================================

static WiFiServer s_bridge(HOST_BRIDGE_PORT);
static WiFiClient s_agent;
static bool     s_begun      = false;
static String   s_rx;                 // bytes read but not yet a complete line
static uint32_t s_nextId     = 1;
static String   s_agentName;          // what the agent called itself on hello
static String   s_agentCaps;          // space-separated op names it supports
static uint32_t s_connectedAt = 0;
static uint32_t s_requests    = 0;

// Unsolicited event lines that arrived while we were waiting for a reply.
// Drained by hostPopEvent() from the main loop so they print like MQTT does.
static std::vector<String> s_events;
static const size_t EVENT_QUEUE_MAX = 8;

// ---- minimal JSON scalar reader --------------------------------------------
// Good enough for the flat, agent-generated objects above and nothing more: it
// does not walk nested structures and does not validate. The agent is the only
// writer of these lines, so the contract is enforced on that side.

// Reads a string field, undoing the escapes the agent is allowed to emit.
static bool jsonField(const String &line, const char *key, String &out) {
  String needle = String("\"") + key + "\":\"";
  int i = line.indexOf(needle);
  if (i < 0) return false;
  i += needle.length();
  out = "";
  out.reserve(line.length() - i);
  for (; i < (int)line.length(); ++i) {
    char c = line[i];
    if (c == '"') return true;            // closing quote: done
    if (c != '\\') { out.concat(c); continue; }
    if (++i >= (int)line.length()) break;
    switch (line[i]) {
      case 'n': out.concat('\n'); break;
      case 'r': out.concat('\r'); break;
      case 't': out.concat('\t'); break;
      case 'u': {                          // \uXXXX: keep ASCII, else '?'
        if (i + 4 >= (int)line.length()) return false;
        long cp = strtol(line.substring(i + 1, i + 5).c_str(), nullptr, 16);
        out.concat((char)(cp > 0 && cp < 0x80 ? (char)cp : '?'));
        i += 4;
        break;
      }
      default: out.concat(line[i]); break;  // \" \\ \/ and anything else
    }
  }
  return false;   // ran off the end without a closing quote
}

// Reads a bare (unquoted) field: a number, or true/false.
static bool jsonNumber(const String &line, const char *key, double &out) {
  String needle = String("\"") + key + "\":";
  int i = line.indexOf(needle);
  if (i < 0) return false;
  i += needle.length();
  if (i < (int)line.length() && line[i] == '"') return false;   // it's a string
  String tok;
  while (i < (int)line.length() && line[i] != ',' && line[i] != '}') tok.concat(line[i++]);
  tok.trim();
  if (tok == "true")  { out = 1; return true; }
  if (tok == "false") { out = 0; return true; }
  if (tok.length() == 0) return false;
  out = atof(tok.c_str());
  return true;
}

static bool jsonBool(const String &line, const char *key) {
  double v = 0;
  return jsonNumber(line, key, v) && v != 0;
}

// Escapes a string for use as a JSON value.
static String jsonEscape(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '"':  o.concat("\\\""); break;
      case '\\': o.concat("\\\\"); break;
      case '\n': o.concat("\\n");  break;
      case '\r': o.concat("\\r");  break;
      case '\t': o.concat("\\t");  break;
      default:
        if ((uint8_t)c < 0x20) {            // other controls: \u00XX
          char b[7];
          snprintf(b, sizeof(b), "\\u%04X", (unsigned)(uint8_t)c);
          o.concat(b);
        } else {
          o.concat(c);
        }
    }
  }
  return o;
}

// ---- connection ------------------------------------------------------------
static void hostDropAgent(const char *why) {
  if (s_agent) s_agent.stop();
  s_rx = "";
  s_agentName = "";
  s_agentCaps = "";
  s_connectedAt = 0;
  (void)why;
}

void hostBridgeBegin() {
  if (s_begun) return;
  s_bridge.begin();
  s_bridge.setNoDelay(true);
  s_begun = true;
}

// Pulls whole lines off the socket. Returns false once the peer has gone away.
// Lines that are not a reply to `waitId` are events, and get queued.
static bool hostDrain(uint32_t waitId, String &reply, bool &gotReply) {
  while (s_agent.available()) {
    char c = (char)s_agent.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (s_rx.length() < HOST_BRIDGE_LINE_MAX) s_rx.concat(c);
      continue;                              // else: over-long line, keep eating
    }

    String line = s_rx;
    s_rx = "";
    line.trim();
    if (line.length() == 0) continue;

    double id = 0;
    if (waitId && jsonNumber(line, "id", id) && (uint32_t)id == waitId) {
      reply = line;
      gotReply = true;
      continue;
    }
    if (s_events.size() < EVENT_QUEUE_MAX) s_events.push_back(line);
  }
  return s_agent.connected();
}

void hostBridgePoll() {
  if (!s_begun) return;

  if (s_bridge.hasClient()) {
    WiFiClient fresh = s_bridge.available();
    if (s_agent && s_agent.connected()) {
      // One agent at a time: a second laptop would race on the same rules.
      fresh.println(F("{\"ok\":false,\"err\":\"bridge busy\"}"));
      fresh.stop();
    } else {
      hostDropAgent("replaced");
      s_agent = fresh;
      s_agent.setNoDelay(true);
      s_connectedAt = millis();
    }
  }

  if (!s_agent) return;
  if (!s_agent.connected()) { hostDropAgent("closed"); return; }

  String unused;
  bool got = false;
  hostDrain(0, unused, got);          // id 0 never matches: everything is an event

  // The agent's hello is just an event; lift the identity out of it once.
  for (size_t i = 0; i < s_events.size();) {
    String ev;
    if (jsonField(s_events[i], "ev", ev) && ev == "hello") {
      jsonField(s_events[i], "agent", s_agentName);
      jsonField(s_events[i], "caps", s_agentCaps);
      s_events.erase(s_events.begin() + i);
    } else {
      ++i;
    }
  }
}

bool hostConnected() { return s_agent && s_agent.connected(); }

String hostPeer() {
  if (!hostConnected()) return String("");
  return s_agent.remoteIP().toString();
}

bool hostPopEvent(String &text) {
  while (!s_events.empty()) {
    String line = s_events.front();
    s_events.erase(s_events.begin());
    String t;
    if (jsonField(line, "text", t)) { text = t; return true; }
  }
  return false;
}

// ---- request / reply -------------------------------------------------------
bool hostAsk(const String &op, const String &args, String &text, double *val, uint32_t timeoutMs) {
  if (!hostConnected()) {
    text = F("no host agent connected - run tools/espehost.py on the laptop (see 'host')");
    return false;
  }

  const uint32_t id = s_nextId++;
  if (s_nextId == 0) s_nextId = 1;          // wrap past the 0 sentinel

  String req = "{\"id\":";
  req += id;
  req += ",\"op\":\"";
  req += jsonEscape(op);
  req += "\"";
  if (args.length()) {
    req += ",\"args\":\"";
    req += jsonEscape(args);
    req += "\"";
  }
  req += "}\n";

  if (s_agent.print(req) != req.length()) {
    hostDropAgent("write failed");
    text = F("host: link died while sending");
    return false;
  }
  s_requests++;

  const uint32_t deadline = millis() + timeoutMs;
  String reply;
  bool gotReply = false;
  while (!gotReply) {
    if (!hostDrain(id, reply, gotReply)) {
      if (!gotReply) { hostDropAgent("closed mid-request"); text = F("host: agent disconnected"); return false; }
      break;
    }
    if (gotReply) break;
    if ((int32_t)(millis() - deadline) >= 0) {
      text = F("host: timed out waiting for the agent");
      return false;
    }
    delay(2);
  }

  if (val) { *val = 0; jsonNumber(reply, "val", *val); }

  if (!jsonBool(reply, "ok")) {
    if (!jsonField(reply, "err", text)) text = F("host: agent reported an error");
    return false;
  }
  if (!jsonField(reply, "text", text)) text = "";
  return true;
}

// Convenience wrapper for rules and scripts that want the number, not the prose.
bool hostAskNumber(const String &op, const String &args, double &value) {
  String text;
  return hostAsk(op, args, text, &value, HOST_BRIDGE_TIMEOUT_MS);
}

// ---- command plumbing ------------------------------------------------------
// Joins argv[from..] back into one string for the agent to parse. The agent
// gets the flags verbatim so new options never need a reflash here.
static String joinArgs(int argc, char **argv, int from) {
  String s;
  for (int i = from; i < argc; ++i) {
    if (s.length()) s.concat(' ');
    s.concat(argv[i]);
  }
  return s;
}

// The body of every h* command: ask the agent, print whatever it says.
int hostRun(const String &op, int argc, char **argv, int firstArg, ShellIO &io) {
  String text;
  bool ok = hostAsk(op, joinArgs(argc, argv, firstArg), text, nullptr, HOST_BRIDGE_TIMEOUT_MS);
  if (text.length()) {
    io.out.print(text);
    if (text[text.length() - 1] != '\n') io.out.println();
  }
  return ok ? 0 : 1;
}

// ---- host ------------------------------------------------------------------
static void hostPrintStatus(ShellIO &io) {
  io.out.print(F("bridge   : listening on port "));
  io.out.println(HOST_BRIDGE_PORT);
  if (!hostConnected()) {
    io.out.println(F("agent    : not connected"));
    io.out.println();
    io.out.println(F("Start the agent on the laptop:"));
    io.out.print(F("    python tools/espehost.py "));
    io.out.println(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(F("<esp32-ip>")));
    return;
  }
  io.out.print(F("agent    : "));
  io.out.println(s_agentName.length() ? s_agentName : String(F("(unnamed)")));
  io.out.print(F("peer     : "));
  io.out.println(hostPeer());
  io.out.print(F("up       : "));
  io.out.print((millis() - s_connectedAt) / 1000);
  io.out.println(F("s"));
  io.out.print(F("requests : "));
  io.out.println(s_requests);
  io.out.print(F("caps     : "));
  io.out.println(s_agentCaps.length() ? s_agentCaps : String(F("(none reported)")));
}

static int cmd_host(int argc, char **argv, ShellIO &io) {
  if (argc < 2 || strcmp(argv[1], "status") == 0) { hostPrintStatus(io); return 0; }

  if (strcmp(argv[1], "caps") == 0) {
    if (!hostConnected()) { io.out.println(F("no host agent connected")); return 1; }
    return hostRun("caps", argc, argv, 2, io);
  }
  if (strcmp(argv[1], "drop") == 0) {
    if (!hostConnected()) { io.out.println(F("no host agent connected")); return 1; }
    hostDropAgent("user");
    io.out.println(F("agent disconnected"));
    return 0;
  }
  if (strcmp(argv[1], "raw") == 0) {
    if (argc < 3) { io.out.println(F("usage: host raw <op> [args...]")); return 1; }
    // Escape hatch: send any op the agent understands, including ones added
    // to the agent after this firmware was flashed.
    return hostRun(argv[2], argc, argv, 3, io);
  }

  io.out.println(F("usage: host [status|caps|raw <op> [args]|drop]"));
  return 1;
}

// ---- the h* family ---------------------------------------------------------
// Each of these is the same three lines - ask the agent, print the reply - so
// rather than eighteen near-identical functions there is one that looks up the
// op by the name it was invoked as. Aliases (hbat -> power) fall out for free,
// and adding a command is one row here plus one in the table below.
static const struct { const char *cmd; const char *op; } HOST_OPS[] = {
  {"hping",   "ping"},
  {"hsys",    "sys"},
  {"hcpu",    "cpu"},
  {"hmem",    "mem"},
  {"hdisk",   "disk"},
  {"hpower",  "power"},
  {"hbat",    "power"},
  {"hcam",    "cam"},
  {"hscreen", "screen"},
  {"hio",     "io"},
  {"hnet",    "net"},
  {"hproc",   "proc"},
  {"hgpu",    "gpu"},
  {"htemp",   "temp"},
  {"hnotify", "notify"},
  {"hexec",   "exec"},
  {"htype",   "type"},
  {"hkey",    "key"},
  {"hclip",   "clip"},
};

static int cmd_hostOp(int argc, char **argv, ShellIO &io) {
  for (const auto &entry : HOST_OPS)
    if (strcmp(entry.cmd, argv[0]) == 0) return hostRun(entry.op, argc, argv, 1, io);
  // Only reachable if the table above and the one below disagree.
  io.out.print(argv[0]);
  io.out.println(F(": not mapped to a host op"));
  return 1;
}

const Command HOST_CMDS[] = {
  {"host",    cmd_host,   "host [status|caps|raw <op>|drop]", "laptop bridge link status", G_HOST},
  {"hping",   cmd_hostOp, "hping",                 "round-trip test to the laptop agent",  G_HOST},
  {"hsys",    cmd_hostOp, "hsys",                  "laptop OS, arch, uptime",              G_HOST},
  {"hcpu",    cmd_hostOp, "hcpu",                  "laptop CPU model and per-core load",   G_HOST},
  {"hmem",    cmd_hostOp, "hmem",                  "laptop RAM and swap usage",            G_HOST},
  {"hdisk",   cmd_hostOp, "hdisk",                 "laptop filesystems and free space",    G_HOST},
  {"hpower",  cmd_hostOp, "hpower [--watts]",      "laptop battery, charge, current draw", G_HOST},
  {"hbat",    cmd_hostOp, "hbat",                  "laptop battery (alias of hpower)",     G_HOST},
  {"hcam",    cmd_hostOp, "hcam [list|snap|ascii]","laptop cameras: list, snap, preview",  G_HOST},
  {"hscreen", cmd_hostOp, "hscreen [-w N] [-o f]", "laptop display as ASCII, or to PNG",   G_HOST},
  {"hio",     cmd_hostOp, "hio [usb|hid|audio|serial|..]", "laptop input/output devices",  G_HOST},
  {"hnet",    cmd_hostOp, "hnet [conn] [-a]",      "laptop interfaces and connections",    G_HOST},
  {"hproc",   cmd_hostOp, "hproc [N] [-m]",        "laptop top processes",                 G_HOST},
  {"hgpu",    cmd_hostOp, "hgpu",                  "laptop graphics adapters",             G_HOST},
  {"htemp",   cmd_hostOp, "htemp",                 "laptop thermal sensors and fans",      G_HOST},
  {"hnotify", cmd_hostOp, "hnotify <text>",        "pop a notification on the laptop",     G_HOST},
  {"hexec",   cmd_hostOp, "hexec <cmd...>",        "run a command (agent --allow-exec)",   G_HOST},
  {"htype",   cmd_hostOp, "htype <text>",          "type text (agent --allow-input)",      G_HOST},
  {"hkey",    cmd_hostOp, "hkey <key...>",         "press keys (agent --allow-input)",     G_HOST},
  {"hclip",   cmd_hostOp, "hclip [text]",          "clipboard (agent --allow-input)",      G_HOST},
};
const size_t HOST_CMDS_N = sizeof(HOST_CMDS) / sizeof(HOST_CMDS[0]);
