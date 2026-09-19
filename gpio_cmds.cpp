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

const Command GPIO_CMDS[] = {
  {"adc", cmd_adc, "adc <pin> [-n N] [-d ms]", "read an analog input (raw + mV)", G_ESP},
};
const size_t GPIO_CMDS_N = sizeof(GPIO_CMDS) / sizeof(GPIO_CMDS[0]);
