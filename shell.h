#pragma once
#include <Arduino.h>
#include <vector>
#include <utility>

// ============================================================================
//  ESPEShell - shell core interface
// ============================================================================

// ---- I/O abstraction -------------------------------------------------------
// Every command writes to `out`. If the command was on the right side of a
// pipe, `in` points at the previous stage's captured output; otherwise null.
// `rawIn`, when non-null, is the live underlying connection (Serial or the
// current Telnet client) - used by commands like `recv` that need to read
// raw pasted bytes directly rather than piped shell text.
struct ShellIO {
  Print &out;
  const String *in;
  Stream *rawIn;
  ShellIO(Print &o, const String *i = nullptr, Stream *r = nullptr) : out(o), in(i), rawIn(r) {}
  bool hasIn() const { return in != nullptr; }
};

typedef int (*CmdFn)(int argc, char **argv, ShellIO &io);

// Command groups (used to bucket the `help` output).
enum {
  G_FS = 0,
  G_TEXT,
  G_SEARCH,
  G_SYS,
  G_PROC,
  G_NET,
  G_ENV,
  G_ARCHIVE,
  G_ESP,
  G_CORE,
  G_XFER,
  G_COUNT
};

struct Command {
  const char *name;
  CmdFn fn;
  const char *usage;    // e.g. "ls [-l] [-a] [path]"
  const char *summary;  // one line
  uint8_t group;
};

// A module exposes an array of Commands; shell.cpp aggregates them.
struct CmdTable {
  const Command *cmds;
  size_t count;
};

// ---- Command tables provided by the various *_cmds.cpp modules -------------
extern const Command CORE_CMDS[];    extern const size_t CORE_CMDS_N;
extern const Command FS_CMDS[];      extern const size_t FS_CMDS_N;
extern const Command TEXT_CMDS[];    extern const size_t TEXT_CMDS_N;
extern const Command SEARCH_CMDS[];  extern const size_t SEARCH_CMDS_N;
extern const Command SYS_CMDS[];     extern const size_t SYS_CMDS_N;
extern const Command NET_CMDS[];     extern const size_t NET_CMDS_N;
extern const Command MISC_CMDS[];    extern const size_t MISC_CMDS_N;
extern const Command ESP_CMDS[];     extern const size_t ESP_CMDS_N;
extern const Command XFER_CMDS[];    extern const size_t XFER_CMDS_N;

// The aggregate, built in shell.cpp. Grows as modules are added.
extern const CmdTable CMD_TABLES[];
extern const size_t CMD_TABLE_COUNT;

const char *groupName(uint8_t g);

// ---- Shell state (defined in shell.cpp) ------------------------------------
extern String g_cwd;          // current dir, always absolute, no trailing '/'
extern String g_hostname;     // prompt host
extern String g_user;         // prompt user / whoami
extern String g_telnetPeer;   // IP of the connected telnet client, or ""
extern String g_telnetPassword; // current telnet login password (changeable at runtime)
extern volatile uint32_t g_bytesIn;   // bytes received across sessions
extern volatile uint32_t g_bytesOut;  // bytes sent across sessions

// ---- Environment variables -------------------------------------------------
String envGet(const String &key);
void   envSet(const String &key, const String &val);
bool   envUnset(const String &key);
const std::vector<std::pair<String, String>> &envAll();

// ---- Command history (interactive lines only - see ESPEShell.ino) ---------
void   historyAdd(const String &line);
size_t historyCount();
String historyGet(int indexFromEnd);   // 0 = most recently added

// ---- Tab completion ---------------------------------------------------------
// Completes the last token of `partial`: a command name if it's the first
// token, else a path relative to the cwd. Returns the completed line on a
// unique match; on multiple matches it prints the candidates to `out` and
// returns `partial` unchanged; on no match it also returns `partial` unchanged.
String completeLine(const String &partial, Print &out);

// ---- Path helpers ----------------------------------------------------------
String normalizePath(const String &path);          // collapse . .. //, leading /
String resolvePath(const String &p);               // relative to g_cwd
String baseName(const String &p);
String dirName(const String &p);
bool   pathExists(const String &abs);
bool   isDir(const String &abs);

// ---- Shared command helpers ------------------------------------------------
// Load piped stdin, or the concatenation of file args (from firstFileArg on),
// into `out`. Returns true if any input source was found.
bool collectInput(int argc, char **argv, int firstFileArg, ShellIO &io, String &out);
void splitLines(const String &s, std::vector<String> &lines);
bool matchWild(const String &text, const String &pat);   // glob: * and ?
String humanBytes(uint64_t n);
String expandVars(const String &s);

// ---- WiFi (net_cmds.cpp) - used at boot and by the `wifi` command ----------
bool wifiConnect(const String &ssid, const String &pass, unsigned long timeoutMs, Print &out);
void wifiLoadAndConnect(Print &out);   // reads NVS-saved creds, else config.h; called once from setup()

// ---- Boot counter (esp_cmds.cpp) - persisted in NVS, used by `dmesg` -------
// Increments once per boot (cached after the first call); call once from
// setup() so the count is correct even if `dmesg` is never run.
uint32_t espeBootCount();

// ---- Dispatch --------------------------------------------------------------
const Command *findCommand(const char *name);
int  runLine(const String &line, Print &realOut, Stream *rawIn = nullptr);   // pipes + redirection
void printPrompt(Print &out);
void printBanner(Print &out);
