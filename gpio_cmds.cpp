#include "shell.h"
#include "config.h"
#include <WiFi.h>
#include "soc/soc_caps.h"

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

const Command GPIO_CMDS[] = {
  {"tone", cmd_tone, "tone <pin> <hz> [ms]|off", "square-wave tone on a pin",         G_ESP},
  {"beep", cmd_beep, "beep [pin] [ms]",          "short 1 kHz beep (default GPIO13)", G_ESP},
  {"dac", cmd_dac, "dac <25|26> <0-255|off>",  "analog output voltage (8-bit DAC)", G_ESP},
  {"adc", cmd_adc, "adc <pin> [-n N] [-d ms]", "read an analog input (raw + mV)", G_ESP},
};
const size_t GPIO_CMDS_N = sizeof(GPIO_CMDS) / sizeof(GPIO_CMDS[0]);
