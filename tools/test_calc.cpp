// Native unit test for the bc expression parser in calc_cmds.cpp.
//
//   g++ -std=gnu++17 -O1 -o test_calc tools/test_calc.cpp && ./test_calc
//
// calc_cmds.cpp is Arduino code, so the parser cannot be linked directly.
// Instead the Calc struct is sliced out of the real source at build time by
// tools/test_calc.sh and #included here, which means this tests the shipping
// parser rather than a copy that can drift away from it.
//
// Only Arduino's String and isalpha/isdigit are needed, so they are shimmed.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// ---- the sliver of the Arduino API the parser touches ----------------------
struct String : std::string {
    String() {}
    String(const char *s) : std::string(s ? s : "") {}
    String(const std::string &s) : std::string(s) {}
    void concat(char c) { push_back(c); }
    unsigned length() const { return (unsigned)size(); }
};
static inline bool operator==(const String &a, const char *b) { return (const std::string &)a == b; }

#include "calc_generated.inc"   // the Calc struct, sliced from calc_cmds.cpp

// ---- harness ---------------------------------------------------------------
static int failures = 0;
static int checks = 0;

static void expect(const char *expr, double want) {
    checks++;
    Calc c{expr};
    double got = c.parse();
    if (!c.ok) {
        printf("  FAIL  %-28s -> error \"%s\" (wanted %g)\n", expr, c.err.c_str(), want);
        failures++;
        return;
    }
    if (std::fabs(got - want) > 1e-9 * (std::fabs(want) > 1 ? std::fabs(want) : 1)) {
        printf("  FAIL  %-28s -> %.10g (wanted %.10g)\n", expr, got, want);
        failures++;
        return;
    }
    printf("  ok    %-28s =  %g\n", expr, got);
}

static void expect_error(const char *expr) {
    checks++;
    Calc c{expr};
    double got = c.parse();
    if (c.ok) {
        printf("  FAIL  %-28s -> %g (wanted an error)\n", expr, got);
        failures++;
        return;
    }
    printf("  ok    %-28s -> error: %s\n", expr, c.err.c_str());
}

int main() {
    printf("precedence and associativity\n");
    expect("1+2*3", 7);
    expect("(1+2)*3", 9);
    expect("2+3*4-6/2", 11);
    expect("100/10/2", 5);            // left-associative
    expect("2**3**2", 512);           // right-associative
    expect("-2**2", -4);              // unary binds looser than **
    expect("2*-3", -6);
    expect("--5", 5);
    expect("10%3", 1);
    expect("7/2", 3.5);               // real division, not integer

    printf("\nbitwise (truncating to integer, as bc does)\n");
    expect("6&3", 2);
    expect("6|3", 7);
    expect("6^3", 5);
    expect("~0", -1);
    expect("1<<10", 1024);
    expect("1024>>3", 128);
    expect("1|2&3", 3);               // & binds tighter than |
    expect("1<<2+1", 8);              // + binds tighter than <<

    printf("\nliterals\n");
    expect("0xff", 255);
    expect("0xFF+1", 256);
    expect("0b1011", 11);
    expect(".5", 0.5);
    expect("1e3", 1000);
    expect("  42  ", 42);

    printf("\nfunctions and constants\n");
    expect("sqrt(16)", 4);
    expect("abs(-7)", 7);
    expect("int(3.9)", 3);
    expect("round(3.5)", 4);
    expect("floor(-1.5)", -2);
    expect("ceil(1.1)", 2);
    expect("log2(1024)", 10);
    expect("log10(1000)", 3);
    expect("exp(0)", 1);
    expect("sqrt(2)**2", 2);
    expect("pi", M_PI);
    expect("e", M_E);
    expect("sqrt(9)+abs(-1)*2", 5);

    printf("\nerrors are reported, not guessed at\n");
    expect_error("1/0");
    expect_error("10%0");
    expect_error("sqrt(-1)");
    expect_error("log(0)");
    expect_error("(1+2");
    expect_error("1+");
    expect_error("");
    expect_error("2 3");              // trailing input, not silently ignored
    expect_error("nosuchfn(1)");
    expect_error("bogus");
    expect_error("0xzz");

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
