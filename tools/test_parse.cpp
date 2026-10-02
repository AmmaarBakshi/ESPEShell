// Native unit test for the command-line parser in shell.cpp.
//
//   bash tools/test_parse.sh
//
// parseDollar(), splitPipes(), tokenize() and splitList() are sliced out of
// the real shell.cpp at build time and #included here, so this tests the
// shipping parser. They need Arduino's String, envGet() and the $? status,
// which are shimmed below.

#include <cctype>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// ---- the sliver of the Arduino API the parser touches ----------------------
struct String : std::string {
    String() {}
    String(const char *s) : std::string(s ? s : "") {}
    String(const std::string &s) : std::string(s) {}
    explicit String(int v) : std::string(std::to_string(v)) {}
    unsigned length() const { return (unsigned)size(); }
    String substring(size_t a, size_t b) const { return String(substr(a, b - a)); }
    bool endsWith(const char *s) const {
        size_t n = std::char_traits<char>::length(s);
        return size() >= n && compare(size() - n, n, s) == 0;
    }
    void remove(size_t i) { erase(i); }
};

static int s_lastStatus = 3;
static std::vector<std::pair<std::string, std::string>> s_env = {
    {"X", "1 2"}, {"HOME", "/"}, {"EMPTY", ""}, {"NAME", "esp"},
};
static String envGet(const String &key) {
    for (auto &kv : s_env)
        if (kv.first == key) return String(kv.second);
    return String();
}

#include "parse_generated.inc"   // parseDollar .. splitList, from shell.cpp

// ---- harness ---------------------------------------------------------------
static int failures = 0;
static int checks = 0;

static std::string show(const std::vector<std::string> &v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += "\"" + v[i] + "\"";
    }
    return s + "]";
}

static void report(const char *what, const char *in, const std::vector<std::string> &got,
                   const std::vector<std::string> &want) {
    checks++;
    if (got == want) {
        printf("  ok    %-6s %-26s -> %s\n", what, in, show(got).c_str());
        return;
    }
    printf("  FAIL  %-6s %-26s -> %s (wanted %s)\n", what, in, show(got).c_str(),
           show(want).c_str());
    failures++;
}

static void tok(const char *line, std::vector<std::string> want) {
    std::vector<String> args;
    tokenize(String(line), args);
    std::vector<std::string> got(args.begin(), args.end());
    report("tok", line, got, want);
}

// Items are rendered as "<op>text", with '.' standing for "first item".
static void list(const char *line, std::vector<std::string> want) {
    std::vector<ListItem> items;
    splitList(String(line), items);
    std::vector<std::string> got;
    for (auto &it : items) got.push_back(std::string(1, it.op ? it.op : '.') + it.text);
    report("list", line, got, want);
}

static void pipes(const char *line, std::vector<std::string> want) {
    std::vector<String> segs = splitPipes(String(line));
    std::vector<std::string> got(segs.begin(), segs.end());
    report("pipe", line, got, want);
}

int main() {
    printf("command lists\n");
    list("a; b", {".a", "; b"});
    list("a && b || c", {".a ", "& b ", "| c"});
    list("echo 'a;b' \"c&&d\"", {".echo 'a;b' \"c&&d\""});
    list("echo a\\;b", {".echo a\\;b"});
    list("a | b", {".a | b"});
    list("ls ;", {".ls ", ";"});
    list("echo \"a\\\";b\"", {".echo \"a\\\";b\""});

    printf("pipes\n");
    pipes("ls | wc", {"ls ", " wc"});
    pipes("echo a\\|b | wc", {"echo a\\|b ", " wc"});
    pipes("echo '|' | wc", {"echo '|' ", " wc"});

    printf("tokens and expansion\n");
    tok("echo $X", {"echo", "1", "2"});
    tok("echo \"$X\"", {"echo", "1 2"});
    tok("echo '$X'", {"echo", "$X"});
    tok("echo ${NAME}32", {"echo", "esp32"});
    tok("echo $NAME.local", {"echo", "esp.local"});
    tok("echo $?", {"echo", "3"});
    tok("awk \"{print $1}\"", {"awk", "{print $1}"});
    tok("grep x$", {"grep", "x$"});
    tok("echo \\$X", {"echo", "$X"});
    tok("echo \"\\$X\"", {"echo", "$X"});
    tok("echo $EMPTY a", {"echo", "a"});
    tok("echo \"$EMPTY\"", {"echo", ""});
    tok("echo $UNSET", {"echo"});
    tok("echo ${X", {"echo", "${X"});
    tok("echo $", {"echo", "$"});
    tok("cd ~", {"cd", "/"});
    tok("cat ~/f", {"cat", "/f"});
    tok("echo a~b ~x", {"echo", "a~b", "~x"});
    tok("echo \"~\"", {"echo", "~"});
    tok("echo 'a b'  c", {"echo", "a b", "c"});

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
