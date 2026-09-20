#include "shell.h"
#include "config.h"
#include <LittleFS.h>
#include <WiFi.h>

// ============================================================================
//  fuse - fusion mode: the laptop's sensors driving the ESP32's pins, and the
//  ESP32's pins driving the laptop.
//
//  A rule is  <source> <comparator> <threshold> : <shell command>. The source
//  may be anything on this board (a pin, the ADC, free heap, die temperature)
//  or anything the laptop agent can measure (CPU load, battery, memory), and
//  the action is an ordinary shell line - so either side can trigger the other:
//
//      fuse add host.cpu gt 80 led on            # laptop busy  -> LED on
//      fuse add adc.34 gt 2000 hnotify "light!"  # ESP32 sensor -> laptop toast
//      fuse add host.power lt 20 pwm 13 255      # battery low  -> buzzer
//
//  Comparators are words (gt lt ge le eq ne), not symbols. '>' would be eaten
//  by the shell's output-redirection parser long before this command saw it.
//
//  Rules are edge-triggered: the action runs when the condition becomes true,
//  not on every poll while it stays true. --level opts into the latter.
//
//  Evaluation happens from the main loop. Board-local sources are free to read
//  every tick; a host source costs a blocking round-trip to the laptop, and
//  the loop is what serves the shell, so at most one host-backed rule is
//  evaluated per tick, round-robin. With the default interval a set of host
//  rules is fully swept every FUSE_INTERVAL_MS * <number of host rules>.
// ============================================================================

enum { CMP_GT = 0, CMP_LT, CMP_GE, CMP_LE, CMP_EQ, CMP_NE };

static const char *const CMP_WORDS[] = {"gt", "lt", "ge", "le", "eq", "ne"};
static const char *const CMP_SYMS[]  = {">",  "<",  ">=", "<=", "==", "!="};

struct FuseRule {
  String   source;
  uint8_t  cmp;
  double   threshold;
  String   action;
  bool     level;       // fire every poll while true, rather than on the edge
  bool     wasTrue;     // condition state at the previous evaluation
  bool     active;
  uint32_t fires;
  String   lastError;   // why the source could not be read, if it could not
  double   lastValue;
  bool     haveValue;
};

static std::vector<FuseRule> s_rules;
static bool          s_enabled   = false;
static unsigned long s_intervalMs = FUSE_INTERVAL_MS;
static unsigned long s_nextPoll  = 0;
static size_t        s_hostCursor = 0;   // round-robin over host-backed rules
static std::vector<String> s_pending;    // actions waiting for the main loop

static const size_t FUSE_MAX_RULES   = 12;
static const size_t FUSE_PENDING_MAX = 8;

// ---- sources ---------------------------------------------------------------

static bool sourceIsHost(const String &src) { return src.startsWith("host."); }

// Parses "<prefix>.<number>" into `pin`. Returns false if it is not that shape.
static bool sourcePin(const String &src, const char *prefix, int &pin) {
  String want = String(prefix) + ".";
  if (!src.startsWith(want)) return false;
  String rest = src.substring(want.length());
  if (rest.length() == 0) return false;
  for (unsigned i = 0; i < rest.length(); ++i)
    if (!isdigit((int)rest[i])) return false;
  pin = rest.toInt();
  return true;
}

// Reads whatever `src` names into `out`. On failure `err` says why.
static bool fuseRead(const String &src, double &out, String &err) {
  int pin = 0;

  if (sourceIsHost(src)) {
    String op = src.substring(5);
    if (op.length() == 0) { err = F("host. needs an op name"); return false; }
    String text;
    if (!hostAsk(op, "", text, &out, FUSE_HOST_TIMEOUT_MS)) { err = text; return false; }
    return true;
  }
  if (sourcePin(src, "pin", pin)) {
    if (!espePinUsable(pin)) { err = F("pin not usable"); return false; }
    pinMode(pin, INPUT);
    out = digitalRead(pin);
    return true;
  }
  if (sourcePin(src, "adc", pin)) {
    if (!espePinUsable(pin)) { err = F("pin not usable"); return false; }
    out = analogRead(pin);
    return true;
  }
  if (sourcePin(src, "touch", pin)) {
    out = touchRead(pin);
    return true;
  }
  if (src == "heap")   { out = ESP.getFreeHeap(); return true; }
  if (src == "uptime") { out = millis() / 1000.0; return true; }
  if (src == "temp")   { out = temperatureRead(); return true; }
  if (src == "rssi")   { out = WiFi.RSSI(); return true; }

  err = F("unknown source");
  return false;
}

static bool cmpHolds(uint8_t cmp, double a, double b) {
  switch (cmp) {
    case CMP_GT: return a >  b;
    case CMP_LT: return a <  b;
    case CMP_GE: return a >= b;
    case CMP_LE: return a <= b;
    case CMP_EQ: return a == b;
    case CMP_NE: return a != b;
    default:     return false;
  }
}

static bool parseCmp(const String &tok, uint8_t &out) {
  for (uint8_t i = 0; i < 6; ++i)
    if (tok == CMP_WORDS[i] || tok == CMP_SYMS[i]) { out = i; return true; }
  return false;
}

// ---- evaluation ------------------------------------------------------------

static void fuseEvaluate(FuseRule &r) {
  double value = 0;
  String err;
  if (!fuseRead(r.source, value, err)) {
    r.lastError = err;
    r.haveValue = false;
    // A source we cannot read is not a false condition: leave the edge state
    // alone so the link coming back does not re-fire everything at once.
    return;
  }
  r.lastError = "";
  r.lastValue = value;
  r.haveValue = true;

  const bool now = cmpHolds(r.cmp, value, r.threshold);
  const bool fire = r.level ? now : (now && !r.wasTrue);
  r.wasTrue = now;
  if (!fire) return;

  r.fires++;
  if (s_pending.size() < FUSE_PENDING_MAX) s_pending.push_back(r.action);
}

// Called from the main loop. Board-local rules are cheap enough to do every
// tick; host rules cost a round-trip, so one per tick.
void fusePoll() {
  if (!s_enabled || s_rules.empty()) return;
  const unsigned long now = millis();
  if ((long)(now - s_nextPoll) < 0) return;
  s_nextPoll = now + s_intervalMs;

  for (auto &r : s_rules)
    if (r.active && !sourceIsHost(r.source)) fuseEvaluate(r);

  if (!hostConnected()) return;
  for (size_t tried = 0; tried < s_rules.size(); ++tried) {
    size_t i = (s_hostCursor + tried) % s_rules.size();
    if (!s_rules[i].active || !sourceIsHost(s_rules[i].source)) continue;
    fuseEvaluate(s_rules[i]);
    s_hostCursor = (i + 1) % s_rules.size();
    break;
  }
}

bool fusePopDue(String &line) {
  if (s_pending.empty()) return false;
  line = s_pending.front();
  s_pending.erase(s_pending.begin());
  return true;
}

size_t fuseRuleCount() {
  size_t n = 0;
  for (auto &r : s_rules) if (r.active) n++;
  return n;
}

// ---- persistence -----------------------------------------------------------
// One rule per line, in exactly the syntax `fuse add` accepts, so the file is
// editable with the shell's own text commands.

static int fuseSave(Print &out) {
  File f = LittleFS.open(FUSE_RULES_PATH, "w");
  if (!f) { out.println(F("fuse: cannot write " FUSE_RULES_PATH)); return 1; }
  size_t n = 0;
  for (auto &r : s_rules) {
    if (!r.active) continue;
    f.printf("%s %s %g %s: %s\n", r.source.c_str(), CMP_WORDS[r.cmp], r.threshold,
             r.level ? "--level " : "", r.action.c_str());
    n++;
  }
  f.close();
  out.printf("fuse: saved %u rule(s) to %s\n", (unsigned)n, FUSE_RULES_PATH);
  return 0;
}

static int fuseAddParsed(const String &source, const String &cmpTok,
                         const String &thresholdTok, const String &action,
                         bool level, Print &out);

static int fuseLoad(Print &out, bool quiet) {
  String content;
  if (!readFileToString(FUSE_RULES_PATH, content, quiet ? nullptr : &out, FUSE_RULES_PATH))
    return 1;

  std::vector<String> lines;
  splitLines(content, lines);
  size_t loaded = 0;
  for (auto &raw : lines) {
    String line = raw;
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;

    int colon = line.indexOf(':');
    if (colon < 0) continue;
    String cond = line.substring(0, colon);
    String action = line.substring(colon + 1);
    cond.trim();
    action.trim();

    bool level = false;
    if (cond.indexOf("--level") >= 0) {
      level = true;
      cond.replace("--level", "");
      cond.trim();
    }
    int sp1 = cond.indexOf(' ');
    int sp2 = (sp1 >= 0) ? cond.indexOf(' ', sp1 + 1) : -1;
    if (sp1 < 0 || sp2 < 0) continue;

    if (fuseAddParsed(cond.substring(0, sp1), cond.substring(sp1 + 1, sp2),
                      cond.substring(sp2 + 1), action, level,
                      quiet ? Serial : out) == 0) loaded++;
  }
  if (!quiet) out.printf("fuse: loaded %u rule(s)\n", (unsigned)loaded);
  return 0;
}

void fuseLoadAtBoot() {
  if (!LittleFS.exists(FUSE_RULES_PATH)) return;
  fuseLoad(Serial, true);
  if (fuseRuleCount()) {
    s_enabled = true;
    s_nextPoll = millis() + s_intervalMs;
    Serial.printf("[fuse] %u rule(s) loaded, fusion mode on\n", (unsigned)fuseRuleCount());
  }
}

// ---- command ---------------------------------------------------------------

static int fuseAddParsed(const String &source, const String &cmpTok,
                         const String &thresholdTok, const String &action,
                         bool level, Print &out) {
  uint8_t cmp = 0;
  if (!parseCmp(cmpTok, cmp)) {
    out.print(cmpTok);
    out.println(F(": not a comparator (gt lt ge le eq ne)"));
    return 1;
  }
  if (action.length() == 0) { out.println(F("fuse: no action given")); return 1; }
  if (fuseRuleCount() >= FUSE_MAX_RULES) {
    out.println(F("fuse: too many rules - remove one first"));
    return 1;
  }
  // A rule whose action adds rules, or that blocks forever, would wedge the
  // loop that evaluates it.
  if (action.startsWith("fuse ") || action.startsWith("watch ") || action.startsWith("every ")) {
    out.println(F("fuse: that action would never finish - pick a one-shot command"));
    return 1;
  }

  FuseRule r;
  r.source    = source;
  r.cmp       = cmp;
  r.threshold = atof(thresholdTok.c_str());
  r.action    = action;
  r.level     = level;
  r.wasTrue   = false;
  r.active    = true;
  r.fires     = 0;
  r.lastValue = 0;
  r.haveValue = false;
  s_rules.push_back(r);
  return 0;
}

static void fuseList(ShellIO &io) {
  if (s_rules.empty()) {
    io.out.println(F("no fusion rules   (fuse add <source> <gt|lt|..> <value> <command...>)"));
    return;
  }
  io.out.println(F("ID  SOURCE        COND        FIRES  NOW        ACTION"));
  for (size_t i = 0; i < s_rules.size(); ++i) {
    FuseRule &r = s_rules[i];
    if (!r.active) continue;
    char cond[20];
    snprintf(cond, sizeof(cond), "%s %g", CMP_WORDS[r.cmp], r.threshold);
    String now = r.lastError.length() ? String("-") :
                 r.haveValue ? String(r.lastValue, 1) : String("?");
    io.out.printf("%-3u %-13s %-11s %-6lu %-10s %s%s\n", (unsigned)i,
                  r.source.c_str(), cond, (unsigned long)r.fires, now.c_str(),
                  r.level ? "[level] " : "", r.action.c_str());
    if (r.lastError.length()) {
      io.out.print(F("      last error: "));
      io.out.println(r.lastError);
    }
  }
}

static int cmd_fuse(int argc, char **argv, ShellIO &io) {
  String sub = (argc >= 2) ? String(argv[1]) : String("status");

  if (sub == "status") {
    io.out.print(F("fusion   : "));
    io.out.println(s_enabled ? F("on") : F("off"));
    io.out.print(F("rules    : "));
    io.out.println(fuseRuleCount());
    io.out.print(F("interval : "));
    io.out.print(s_intervalMs / 1000);
    io.out.println(F("s"));
    io.out.print(F("agent    : "));
    io.out.println(hostConnected() ? hostPeer() : String(F("not connected")));
    io.out.println();
    fuseList(io);
    return 0;
  }
  if (sub == "list" || sub == "-l") { fuseList(io); return 0; }

  if (sub == "on")  {
    if (s_rules.empty()) { io.out.println(F("fuse: no rules to run")); return 1; }
    s_enabled = true; s_nextPoll = millis();
    io.out.println(F("fusion mode on"));
    return 0;
  }
  if (sub == "off") { s_enabled = false; io.out.println(F("fusion mode off")); return 0; }

  if (sub == "add") {
    // fuse add <source> <cmp> <value> [--level] <command...>
    if (argc < 6) {
      io.out.println(F("usage: fuse add <source> <gt|lt|ge|le|eq|ne> <value> [--level] <command...>"));
      io.out.println(F("   eg: fuse add host.cpu gt 80 led on"));
      return 1;
    }
    int at = 5;
    bool level = false;
    if (String(argv[at]) == "--level") { level = true; at++; }
    String action;
    for (int i = at; i < argc; ++i) { if (action.length()) action += ' '; action += argv[i]; }

    if (fuseAddParsed(argv[2], argv[3], argv[4], action, level, io.out) != 0) return 1;
    io.out.printf("fuse: rule %u added\n", (unsigned)(s_rules.size() - 1));
    if (!s_enabled) io.out.println(F("      (fusion mode is off - 'fuse on' to start)"));
    return 0;
  }

  if (sub == "del" || sub == "-d") {
    if (argc < 3) { io.out.println(F("usage: fuse del <id>")); return 1; }
    size_t id = (size_t)String(argv[2]).toInt();
    if (id >= s_rules.size() || !s_rules[id].active) { io.out.println(F("fuse: no such rule")); return 1; }
    s_rules[id].active = false;
    io.out.printf("fuse: rule %u removed\n", (unsigned)id);
    return 0;
  }

  if (sub == "clear") {
    size_t n = fuseRuleCount();
    s_rules.clear();
    s_enabled = false;
    io.out.printf("fuse: %u rule(s) removed\n", (unsigned)n);
    return 0;
  }

  if (sub == "every") {
    if (argc < 3) { io.out.printf("fuse: interval is %lus\n", s_intervalMs / 1000); return 0; }
    long secs = String(argv[2]).toInt();
    if (secs < 1) { io.out.println(F("usage: fuse every <secs>")); return 1; }
    s_intervalMs = (unsigned long)secs * 1000UL;
    s_nextPoll = millis() + s_intervalMs;
    io.out.printf("fuse: evaluating every %lds\n", secs);
    return 0;
  }

  if (sub == "test") {
    // Read a source right now, without needing a rule for it.
    if (argc < 3) { io.out.println(F("usage: fuse test <source>")); return 1; }
    double value = 0;
    String err;
    if (!fuseRead(argv[2], value, err)) {
      io.out.print(argv[2]); io.out.print(F(": ")); io.out.println(err);
      return 1;
    }
    io.out.print(argv[2]);
    io.out.print(F(" = "));
    io.out.println(value, 3);
    return 0;
  }

  if (sub == "save") return fuseSave(io.out);
  if (sub == "load") return fuseLoad(io.out, false);

  if (sub == "sources") {
    io.out.println(F("Board sources:"));
    io.out.println(F("  pin.<n>     digital read (0/1)"));
    io.out.println(F("  adc.<n>     analog read (0-4095)"));
    io.out.println(F("  touch.<n>   capacitive touch reading"));
    io.out.println(F("  heap        free heap, bytes"));
    io.out.println(F("  temp        die temperature, C"));
    io.out.println(F("  rssi        WiFi signal, dBm"));
    io.out.println(F("  uptime      seconds since boot"));
    io.out.println();
    io.out.println(F("Laptop sources (host.<op>, any op the agent reports a number for):"));
    io.out.println(F("  host.cpu    load %        host.mem    used %"));
    io.out.println(F("  host.power  battery %     host.disk   fullest volume %"));
    io.out.println(F("  host.cam    frame mean    host.screen brightness mean"));
    io.out.println(F("  ('host caps' lists everything this agent supports)"));
    return 0;
  }

  io.out.println(F("usage: fuse [status|list|add|del|clear|on|off|every|test|sources|save|load]"));
  return 1;
}

const Command FUSE_CMDS[] = {
  {"fuse", cmd_fuse, "fuse add <src> <cmp> <val> <cmd> | on|off|list|test|sources",
   "fusion mode: laptop sensors <-> ESP32 pins", G_HOST},
};
const size_t FUSE_CMDS_N = sizeof(FUSE_CMDS) / sizeof(FUSE_CMDS[0]);
