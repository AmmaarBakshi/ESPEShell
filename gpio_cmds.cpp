#include "shell.h"
#include "config.h"
#include <WiFi.h>
#include "soc/soc_caps.h"
#if __has_include("esp_arduino_version.h")
#include "esp_arduino_version.h"
#endif

// The LEDC API is pin-based on core 3.x and channel-based on 2.x (same split
// as `pwm` in esp_cmds.cpp).
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
#define ESPE_LEDC_NEW_API 1
#else
#define ESPE_LEDC_NEW_API 0
#endif

// ============================================================================
//  Peripheral commands: the analog and signal-generating side of the GPIOs
//  (adc, dac, tone, servo, touchpin, pinwatch). Plain digital I/O stays in
//  `pin` (esp_cmds.cpp); everything here registers its pin with the same
//  registry so `pin --used` sees it.
// ============================================================================

// ADC1 (GPIO32..39) keeps working while WiFi is on; ADC2 (the rest) is shared
// with the WiFi radio and reads garbage - or fails - while associated.
static bool isAdc1(int p) { return p >= 32 && p <= 39; }
static bool isAdc2(int p) {
  return p == 0 || p == 2 || p == 4 || (p >= 12 && p <= 15) || (p >= 25 && p <= 27);
}

// ---- adc : read an analog pin ----------------------------------------------
static int cmd_adc(int argc, char **argv, ShellIO &io) {
  if (argc < 2) {
    io.out.println(F("usage: adc <pin> [-n samples] [-d ms]"));
    io.out.println(F("       ADC1 pins 32-39 are safe with WiFi on; 0,2,4,12-15,25-27 are ADC2"));
    return 1;
  }
  int pin = atoi(argv[1]);
  int samples = 1, gapMs = 0;
  for (int i = 2; i < argc; ++i) {
    String a = argv[i];
    if (a == "-n" && i + 1 < argc) samples = atoi(argv[++i]);
    else if (a == "-d" && i + 1 < argc) gapMs = atoi(argv[++i]);
    else { io.out.print(F("adc: unknown option ")); io.out.println(a); return 1; }
  }
  if (!espePinUsable(pin) || (!isAdc1(pin) && !isAdc2(pin))) {
    io.out.printf("adc: GPIO%d has no ADC channel\n", pin);
    return 1;
  }
  if (isAdc2(pin) && WiFi.status() == WL_CONNECTED)
    io.out.printf("adc: warning - GPIO%d is on ADC2, which WiFi owns; readings may be unreliable\n", pin);
  if (samples < 1) samples = 1;
  if (samples > 1000) samples = 1000;

  espeMarkPin(pin, PIN_ADC);
  long sum = 0, mvSum = 0, lo = 4095, hi = 0;
  for (int i = 0; i < samples; ++i) {
    int raw = analogRead(pin);
    sum += raw;
    mvSum += analogReadMilliVolts(pin);
    if (raw < lo) lo = raw;
    if (raw > hi) hi = raw;
    if (gapMs > 0 && i + 1 < samples && shellWait(io, gapMs)) {
      io.out.println(F("adc: stopped."));
      samples = i + 1;
      break;
    }
  }
  long avg = sum / samples, mv = mvSum / samples;
  io.out.printf("GPIO%d  raw=%ld  %ld mV  (%.3f V)\n", pin, avg, mv, mv / 1000.0);
  if (samples > 1)
    io.out.printf("        %d samples, min=%ld max=%ld spread=%ld\n", samples, lo, hi, hi - lo);
  return 0;
}

// ---- dac : true analog out on GPIO25 / GPIO26 ------------------------------
// The classic ESP32 has two 8-bit DACs. Unlike `pwm` this is a real voltage,
// not a switching average, so it drives filters and audio directly.
static int cmd_dac(int argc, char **argv, ShellIO &io) {
#if SOC_DAC_SUPPORTED
  if (argc < 3) {
    io.out.println(F("usage: dac <25|26> <0-255|volts like 1.8v|off>"));
    return 1;
  }
  int pin = atoi(argv[1]);
  if (pin != 25 && pin != 26) { io.out.println(F("dac: only GPIO25 and GPIO26 have a DAC")); return 1; }

  String v = argv[2];
  if (v == "off" || v == "0v") {
    dacDisable(pin);
    espeReleasePin(pin);
    io.out.printf("DAC GPIO%d off
", pin);
    return 0;
  }

  int level;
  if (v.endsWith("v") || v.endsWith("V")) {           // dac 25 1.8v
    double volts = v.substring(0, v.length() - 1).toDouble();
    if (volts < 0 || volts > 3.3) { io.out.println(F("dac: volts must be 0.0 - 3.3")); return 1; }
    level = (int)(volts / 3.3 * 255.0 + 0.5);
  } else {
    level = v.toInt();
    if (level < 0 || level > 255) { io.out.println(F("dac: level must be 0 - 255")); return 1; }
  }

  dacWrite(pin, (uint8_t)level);
  espeMarkPin(pin, PIN_DAC);
  io.out.printf("DAC GPIO%d = %d  (~%.2f V)
", pin, level, level * 3.3 / 255.0);
  return 0;
#else
  io.out.println(F("dac: this chip has no DAC"));
  return 1;
#endif
}

// ---- tone / beep : square wave on a pin (buzzer, piezo) --------------------
static int cmd_tone(int argc, char **argv, ShellIO &io) {
  if (argc < 2) {
    io.out.println(F("usage: tone <pin> <freq-hz> [ms]   |   tone <pin> off"));
    io.out.println(F("       tone 13 440 500   -> A4 for half a second"));
    return 1;
  }
  int pin = atoi(argv[1]);
  if (!espePinUsable(pin) || espePinInputOnly(pin)) {
    io.out.printf("tone: GPIO%d cannot drive an output
", pin);
    return 1;
  }
  if (argc >= 3 && String(argv[2]) == "off") {
    noTone(pin);
    espeReleasePin(pin);
    io.out.printf("tone: GPIO%d silenced
", pin);
    return 0;
  }
  if (argc < 3) { io.out.println(F("tone: frequency required")); return 1; }

  long freq = String(argv[2]).toInt();
  if (freq < 20 || freq > 20000) { io.out.println(F("tone: frequency must be 20 - 20000 Hz")); return 1; }
  long ms = (argc >= 4) ? String(argv[3]).toInt() : 0;

  espeMarkPin(pin, PIN_TONE);
  if (ms > 0) {
    io.out.printf("GPIO%d: %ld Hz for %ld ms
", pin, freq, ms);
    tone(pin, (unsigned int)freq);
    bool stopped = shellWait(io, (int)ms);
    noTone(pin);
    espeReleasePin(pin);
    if (stopped) io.out.println(F("tone: stopped."));
  } else {
    tone(pin, (unsigned int)freq);
    io.out.printf("GPIO%d: %ld Hz (running - 'tone %d off' to stop)
", pin, freq, pin);
  }
  return 0;
}

// `beep` is just the onboard-friendly shorthand people reach for first.
static int cmd_beep(int argc, char **argv, ShellIO &io) {
  int pin  = (argc >= 2) ? atoi(argv[1]) : 13;
  long ms  = (argc >= 3) ? String(argv[2]).toInt() : 150;
  char freq[] = "1000";
  char msBuf[12];
  snprintf(msBuf, sizeof(msBuf), "%ld", ms);
  char pinBuf[8];
  snprintf(pinBuf, sizeof(pinBuf), "%d", pin);
  char *fake[4] = { (char *)"tone", pinBuf, freq, msBuf };
  return cmd_tone(4, fake, io);
}

// ---- servo : hobby servo on any output pin (50 Hz, 0.5-2.5 ms pulse) ------
// Driven straight off LEDC at 16-bit resolution, so no servo library is
// needed: one 20 ms frame is 65536 counts, and the pulse width is a fraction
// of that.
#define SERVO_FREQ_HZ   50
#define SERVO_BITS      16
#define SERVO_MIN_US    500      // ~0 degrees
#define SERVO_MAX_US    2500     // ~180 degrees

static int8_t s_servoChan[40];   // core 2.x only: LEDC channel per pin, -1 = none
static bool   s_servoInit = false;

static int cmd_servo(int argc, char **argv, ShellIO &io) {
  if (!s_servoInit) { for (int i = 0; i < 40; ++i) s_servoChan[i] = -1; s_servoInit = true; }

  if (argc < 3) {
    io.out.println(F("usage: servo <pin> <0-180 | 500-2500us | off>"));
    io.out.println(F("       servo 18 90     -> centre"));
    io.out.println(F("       servo 18 1500us -> the same, by pulse width"));
    return 1;
  }
  int pin = atoi(argv[1]);
  if (!espePinUsable(pin) || espePinInputOnly(pin)) {
    io.out.printf("servo: GPIO%d cannot drive an output
", pin);
    return 1;
  }

  String v = argv[2];
  if (v == "off" || v == "detach") {
#if ESPE_LEDC_NEW_API
    ledcDetach(pin);
#else
    ledcDetachPin(pin);
    s_servoChan[pin] = -1;
#endif
    espeReleasePin(pin);
    io.out.printf("servo: GPIO%d released
", pin);
    return 0;
  }

  long pulseUs;
  if (v.endsWith("us")) {
    pulseUs = v.substring(0, v.length() - 2).toInt();
    if (pulseUs < SERVO_MIN_US || pulseUs > SERVO_MAX_US) {
      io.out.printf("servo: pulse must be %d-%d us
", SERVO_MIN_US, SERVO_MAX_US);
      return 1;
    }
  } else {
    long angle = v.toInt();
    if (angle < 0 || angle > 180) { io.out.println(F("servo: angle must be 0 - 180")); return 1; }
    pulseUs = SERVO_MIN_US + (angle * (SERVO_MAX_US - SERVO_MIN_US)) / 180;
  }

  // duty = pulse / frame, where one frame (20 ms at 50 Hz) is 2^bits counts.
  uint32_t duty = (uint32_t)(((uint64_t)pulseUs << SERVO_BITS) / (1000000UL / SERVO_FREQ_HZ));

#if ESPE_LEDC_NEW_API
  if (!ledcAttach(pin, SERVO_FREQ_HZ, SERVO_BITS)) {
    io.out.printf("servo: could not attach GPIO%d
", pin);
    return 1;
  }
  ledcWrite(pin, duty);
#else
  if (s_servoChan[pin] < 0) {
    // Servos live high in the channel range so they don't fight `pwm`.
    static int nextChan = 12;
    if (nextChan > 15) { io.out.println(F("servo: no free LEDC channel")); return 1; }
    s_servoChan[pin] = nextChan++;
    ledcSetup(s_servoChan[pin], SERVO_FREQ_HZ, SERVO_BITS);
    ledcAttachPin(pin, s_servoChan[pin]);
  }
  ledcWrite(s_servoChan[pin], duty);
#endif

  espeMarkPin(pin, PIN_SERVO);
  io.out.printf("servo GPIO%d: %ld us  (~%ld deg)
", pin, pulseUs,
                (pulseUs - SERVO_MIN_US) * 180 / (SERVO_MAX_US - SERVO_MIN_US));
  return 0;
}

// ---- touchpin : capacitive touch read --------------------------------------
// Named `touchpin` rather than `touch` because `touch` already creates files.
static bool isTouchPin(int p) {
  return p == 0 || p == 2 || p == 4 || (p >= 12 && p <= 15) || p == 27 || p == 32 || p == 33;
}

static int cmd_touchpin(int argc, char **argv, ShellIO &io) {
#if SOC_TOUCH_SENSOR_SUPPORTED
  if (argc < 2) {
    io.out.println(F("usage: touchpin <pin> [-n samples]"));
    io.out.print(F("       touch-capable pins: 0 2 4 12 13 14 15 27 32 33"));
    io.out.println();
    return 1;
  }
  int pin = atoi(argv[1]);
  if (!isTouchPin(pin)) { io.out.printf("touchpin: GPIO%d is not a touch pin
", pin); return 1; }

  int samples = 1;
  if (argc >= 4 && String(argv[2]) == "-n") samples = atoi(argv[3]);
  if (samples < 1) samples = 1;
  if (samples > 100) samples = 100;

  espeMarkPin(pin, PIN_TOUCH);
  uint32_t sum = 0, lo = 0xFFFFFFFFu, hi = 0;
  for (int i = 0; i < samples; ++i) {
    uint32_t v = (uint32_t)touchRead(pin);
    sum += v;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  uint32_t avg = sum / samples;
  io.out.printf("touch GPIO%d = %lu", pin, (unsigned long)avg);
  if (samples > 1) io.out.printf("  (min %lu, max %lu, n=%d)", (unsigned long)lo, (unsigned long)hi, samples);
  io.out.println();
  io.out.println(F("(a finger moves the reading a long way - compare touched vs untouched)"));
  return 0;
#else
  io.out.println(F("touchpin: this chip has no touch sensor"));
  return 1;
#endif
}

// ---- pinwatch : print a digital pin's edges as they happen -----------------
static int cmd_pinwatch(int argc, char **argv, ShellIO &io) {
  if (argc < 2) {
    io.out.println(F("usage: pinwatch <pin> [-up|-down] [-t secs]   (Ctrl-C to stop)"));
    return 1;
  }
  int pin = atoi(argv[1]);
  if (!espePinUsable(pin)) { io.out.printf("pinwatch: GPIO%d is not usable
", pin); return 1; }

  int mode = PIN_INPUT;
  long limitSecs = 0;                       // 0 = until Ctrl-C
  for (int i = 2; i < argc; ++i) {
    String a = argv[i];
    if (a == "-up" || a == "--pullup") mode = PIN_PULLUP;
    else if (a == "-down" || a == "--pulldown") mode = -1;
    else if (a == "-t" && i + 1 < argc) limitSecs = String(argv[++i]).toInt();
    else { io.out.print(F("pinwatch: unknown option ")); io.out.println(a); return 1; }
  }

  pinMode(pin, mode == PIN_PULLUP ? INPUT_PULLUP : (mode == -1 ? INPUT_PULLDOWN : INPUT));
  espeMarkPin(pin, mode == PIN_PULLUP ? PIN_PULLUP : PIN_INPUT);

  int last = digitalRead(pin);
  unsigned long start = millis();
  unsigned long edges = 0;
  io.out.printf("watching GPIO%d (now %d) - Ctrl-C to stop
", pin, last);

  for (;;) {
    if (shellWait(io, 2)) break;            // polls at ~500 Hz, Ctrl-C-able
    int v = digitalRead(pin);
    if (v != last) {
      edges++;
      io.out.printf("  %8lu ms  GPIO%d %d -> %d  (%s)
", millis() - start, pin, last, v,
                    v ? "rising" : "falling");
      last = v;
    }
    if (limitSecs > 0 && (millis() - start) >= (unsigned long)limitSecs * 1000UL) break;
  }
  io.out.printf("pinwatch: %lu edge(s) in %lu ms
", edges, millis() - start);
  return 0;
}

const Command GPIO_CMDS[] = {
  {"pinwatch", cmd_pinwatch, "pinwatch <pin> [-up] [-t s]", "log a pin's edges live",  G_ESP},
  {"touchpin", cmd_touchpin, "touchpin <pin> [-n N]", "capacitive touch reading",     G_ESP},
  {"servo", cmd_servo, "servo <pin> <0-180|off>",  "drive a hobby servo (50 Hz PWM)",   G_ESP},
  {"tone", cmd_tone, "tone <pin> <hz> [ms]|off", "square-wave tone on a pin",         G_ESP},
  {"beep", cmd_beep, "beep [pin] [ms]",          "short 1 kHz beep (default GPIO13)", G_ESP},
  {"dac", cmd_dac, "dac <25|26> <0-255|off>",  "analog output voltage (8-bit DAC)", G_ESP},
  {"adc", cmd_adc, "adc <pin> [-n N] [-d ms]", "read an analog input (raw + mV)", G_ESP},
};
const size_t GPIO_CMDS_N = sizeof(GPIO_CMDS) / sizeof(GPIO_CMDS[0]);
