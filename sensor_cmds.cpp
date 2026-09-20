#include "shell.h"
#include "config.h"
#include <SPI.h>

// ============================================================================
//  Sensor and bus commands - the things you actually wire to a DevKit.
//
//  Every protocol here is bit-banged rather than pulled in from a library, so
//  the sketch keeps its "one optional dependency" property. That costs timing
//  accuracy, which matters: the notes on each command say where it is tight.
// ============================================================================

static int argInt(int argc, char **argv, int n, int fallback) {
  int k = 0;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] == '-' && !isdigit((int)argv[i][1])) continue;
    if (k == n) return atoi(argv[i]);
    k++;
  }
  return fallback;
}

static int intFlag(int argc, char **argv, const char *flag, int fallback) {
  for (int i = 1; i + 1 < argc; ++i)
    if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
  return fallback;
}

static bool checkPin(int pin, ShellIO &io) {
  if (!espePinUsable(pin)) {
    io.out.print(F("pin "));
    io.out.print(pin);
    io.out.println(F(" is not usable (6-11 are wired to the SPI flash)"));
    return false;
  }
  return true;
}

// ---- dht : DHT11 / DHT22 ----------------------------------------------------
// One-wire-ish protocol: pull low >=1ms to start, then the sensor sends 40 bits
// as pulses whose *high* time encodes the value - ~26us for 0, ~70us for 1.
// Timing is read with micros() inside a critical section; a WiFi interrupt
// landing mid-frame is the usual cause of a checksum failure, hence the retry.
static bool dhtRead(int pin, uint8_t out[5]) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delay(20);                        // DHT11 needs >=18ms, DHT22 >=1ms
  digitalWrite(pin, HIGH);
  delayMicroseconds(30);
  pinMode(pin, INPUT_PULLUP);

  uint32_t cycles[83];
  {
    noInterrupts();
    // Sensor answers with 80us low + 80us high, then 40 bits.
    for (int i = 0; i < 83; ++i) {
      uint8_t want = (i & 1) ? HIGH : LOW;
      uint32_t count = 0;
      while (digitalRead(pin) != want)
        if (++count > 20000) { interrupts(); return false; }   // timeout
      uint32_t start = micros();
      while (digitalRead(pin) == want)
        if (micros() - start > 200) break;
      cycles[i] = micros() - start;
    }
    interrupts();
  }

  for (int i = 0; i < 5; ++i) out[i] = 0;
  // cycles[0..2] are the handshake; bit N's high time is at index 4 + 2N.
  for (int bit = 0; bit < 40; ++bit) {
    uint32_t high = cycles[4 + bit * 2];
    out[bit / 8] <<= 1;
    if (high > 45) out[bit / 8] |= 1;      // midpoint between ~26us and ~70us
  }
  return (uint8_t)(out[0] + out[1] + out[2] + out[3]) == out[4];
}

static int cmd_dht(int argc, char **argv, ShellIO &io) {
  int pin = argInt(argc, argv, 0, -1);
  if (pin < 0) { io.out.println(F("usage: dht <pin> [-11|-22]   (default: DHT22)")); return 1; }
  if (!checkPin(pin, io)) return 1;
  const bool dht11 = (strstr(argv[argc - 1], "11") != nullptr) && argc > 2;

  uint8_t raw[5] = {0};
  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
    if (attempt) delay(250);       // the sensor needs ~2s between reads anyway
    ok = dhtRead(pin, raw);
  }
  if (!ok) {
    io.out.println(F("dht: no valid reading (check wiring, a 10k pull-up, and 2s between reads)"));
    return 1;
  }

  float humidity, celsius;
  if (dht11) {
    humidity = raw[0] + raw[1] * 0.1f;
    celsius  = raw[2] + (raw[3] & 0x7F) * 0.1f;
    if (raw[3] & 0x80) celsius = -celsius;
  } else {
    humidity = ((raw[0] << 8) | raw[1]) * 0.1f;
    // DHT22 temperature is sign-and-magnitude, not two's complement.
    celsius  = (((raw[2] & 0x7F) << 8) | raw[3]) * 0.1f;
    if (raw[2] & 0x80) celsius = -celsius;
  }
  io.out.printf("temperature %.1f C  (%.1f F)\n", celsius, celsius * 9.0f / 5.0f + 32.0f);
  io.out.printf("humidity    %.1f %%\n", humidity);
  return 0;
}

// ---- sonar : HC-SR04 ---------------------------------------------------------
static int cmd_sonar(int argc, char **argv, ShellIO &io) {
  int trig = argInt(argc, argv, 0, -1);
  int echo = argInt(argc, argv, 1, -1);
  if (trig < 0 || echo < 0) {
    io.out.println(F("usage: sonar <trig-pin> <echo-pin> [-n N]"));
    io.out.println(F("note: HC-SR04 echo is 5V - use a divider into the ESP32"));
    return 1;
  }
  if (!checkPin(trig, io) || !checkPin(echo, io)) return 1;
  const int samples = intFlag(argc, argv, "-n", 1);

  pinMode(trig, OUTPUT);
  pinMode(echo, INPUT);
  digitalWrite(trig, LOW);

  for (int i = 0; i < samples; ++i) {
    if (i && shellWait(io, 60)) return 0;      // the module needs ~50ms to settle
    delayMicroseconds(4);
    digitalWrite(trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(trig, LOW);

    // 30ms of flight time is about 5m, past this sensor's useful range.
    unsigned long us = pulseIn(echo, HIGH, 30000UL);
    if (us == 0) { io.out.println(F("out of range (no echo)")); continue; }
    // Sound is ~343 m/s at 20 C; halve it because the pulse makes a round trip.
    float cm = us * 0.0343f / 2.0f;
    io.out.printf("%7.1f cm   %6.2f in   (%lu us)\n", cm, cm / 2.54f, us);
  }
  return 0;
}

// ---- pulse : raw pulseIn -----------------------------------------------------
static int cmd_pulse(int argc, char **argv, ShellIO &io) {
  int pin = argInt(argc, argv, 0, -1);
  if (pin < 0) { io.out.println(F("usage: pulse <pin> [-low] [-t ms]")); return 1; }
  if (!checkPin(pin, io)) return 1;
  const bool low = (strstr(argv[argc - 1], "low") != nullptr);
  const unsigned long timeoutUs = (unsigned long)intFlag(argc, argv, "-t", 1000) * 1000UL;

  pinMode(pin, INPUT);
  unsigned long us = pulseIn(pin, low ? LOW : HIGH, timeoutUs);
  if (us == 0) { io.out.println(F("pulse: timed out (no edge)")); return 1; }
  io.out.printf("%lu us   (%.3f ms)\n", us, us / 1000.0);
  return 0;
}

// ---- freq : frequency counter -----------------------------------------------
// Counts edges over a gate window by polling. Polling caps this at roughly
// 100 kHz before sampling starts losing edges, which is fine for a tachometer
// or a 50/60 Hz mains probe and not fine for anything faster.
static int cmd_freq(int argc, char **argv, ShellIO &io) {
  int pin = argInt(argc, argv, 0, -1);
  if (pin < 0) { io.out.println(F("usage: freq <pin> [-t ms]   (polled, good to ~100 kHz)")); return 1; }
  if (!checkPin(pin, io)) return 1;
  const unsigned long windowMs = (unsigned long)intFlag(argc, argv, "-t", 1000);

  pinMode(pin, INPUT);
  unsigned long edges = 0;
  int last = digitalRead(pin);
  const unsigned long start = millis();
  while (millis() - start < windowMs) {
    int now = digitalRead(pin);
    if (now != last && now == HIGH) edges++;    // count rising edges only
    last = now;
  }
  const unsigned long elapsed = millis() - start;
  const double hz = elapsed ? (edges * 1000.0 / elapsed) : 0.0;
  io.out.printf("%lu edge(s) in %lu ms\n", edges, elapsed);
  io.out.printf("%.1f Hz   (%.0f rpm if one pulse per revolution)\n", hz, hz * 60.0);
  return 0;
}

// ---- scope : ASCII plot of an analog pin -------------------------------------
static int cmd_scope(int argc, char **argv, ShellIO &io) {
  int pin = argInt(argc, argv, 0, -1);
  if (pin < 0) {
    io.out.println(F("usage: scope <adc-pin> [-n samples] [-d us] [-h rows]"));
    return 1;
  }
  if (!checkPin(pin, io)) return 1;
  int samples = intFlag(argc, argv, "-n", 64);
  int rows    = intFlag(argc, argv, "-h", 12);
  const int gapUs = intFlag(argc, argv, "-d", 1000);
  if (samples < 2) samples = 2;
  if (samples > 160) samples = 160;      // one screen wide, transposed below
  if (rows < 4) rows = 4;
  if (rows > 24) rows = 24;

  std::vector<int> data;
  data.reserve(samples);
  for (int i = 0; i < samples; ++i) {
    data.push_back(analogRead(pin));
    if (gapUs > 0) delayMicroseconds(gapUs);
  }

  int lo = data[0], hi = data[0];
  long sum = 0;
  for (int v : data) { lo = min(lo, v); hi = max(hi, v); sum += v; }
  // A flat trace would divide by zero and render as a single line at the top;
  // give it a nominal span so it draws along the middle instead.
  const int span = (hi > lo) ? (hi - lo) : 1;

  for (int r = rows - 1; r >= 0; --r) {
    String line;
    line.reserve(samples + 8);
    for (int v : data) {
      int level = (v - lo) * (rows - 1) / span;
      line.concat(level == r ? '*' : (level > r ? '|' : ' '));
    }
    io.out.println(line);
  }
  io.out.printf("min %d  max %d  avg %ld  p-p %d  (%d samples, %d us apart)\n",
                lo, hi, sum / (long)data.size(), hi - lo, samples, gapUs);
  io.out.printf("mV: min %d  max %d\n", (int)(lo * 3300L / 4095), (int)(hi * 3300L / 4095));
  return 0;
}

// ---- logic : multi-pin digital sampler ---------------------------------------
static int cmd_logic(int argc, char **argv, ShellIO &io) {
  std::vector<int> pins;
  for (int i = 1; i < argc; ++i)
    if (isdigit((int)argv[i][0])) pins.push_back(atoi(argv[i]));
  if (pins.empty()) {
    io.out.println(F("usage: logic <pin> [pin...] [-n samples] [-d us]"));
    return 1;
  }
  if (pins.size() > 8) { io.out.println(F("logic: 8 pins maximum")); return 1; }
  for (int p : pins) if (!checkPin(p, io)) return 1;

  int samples = intFlag(argc, argv, "-n", 64);
  const int gapUs = intFlag(argc, argv, "-d", 100);
  if (samples < 2) samples = 2;
  if (samples > 200) samples = 200;

  for (int p : pins) pinMode(p, INPUT);

  // Sample first, render after: printing between samples would dominate the
  // interval and make the timebase meaningless.
  std::vector<uint8_t> frames;
  frames.reserve(samples);
  for (int i = 0; i < samples; ++i) {
    uint8_t bits = 0;
    for (size_t b = 0; b < pins.size(); ++b)
      if (digitalRead(pins[b])) bits |= (1 << b);
    frames.push_back(bits);
    if (gapUs > 0) delayMicroseconds(gapUs);
  }

  for (size_t b = 0; b < pins.size(); ++b) {
    String top, bottom;
    top.reserve(samples);
    bottom.reserve(samples);
    for (uint8_t f : frames) {
      bool high = f & (1 << b);
      top.concat(high ? '_' : ' ');
      bottom.concat(high ? ' ' : '_');
    }
    char label[12];
    snprintf(label, sizeof(label), "IO%-2d ", pins[b]);
    io.out.print(F("     ")); io.out.println(top);
    io.out.print(label);      io.out.println(bottom);
  }
  io.out.printf("%d samples, %d us apart (%.1f ms total)\n",
                samples, gapUs, samples * gapUs / 1000.0);
  return 0;
}

// ---- shiftout : 74HC595 and friends ------------------------------------------
static int cmd_shiftout(int argc, char **argv, ShellIO &io) {
  int dataPin = argInt(argc, argv, 0, -1);
  int clockPin = argInt(argc, argv, 1, -1);
  int latchPin = argInt(argc, argv, 2, -1);
  int value = argInt(argc, argv, 3, -1);
  if (dataPin < 0 || clockPin < 0 || latchPin < 0 || value < 0) {
    io.out.println(F("usage: shiftout <data> <clock> <latch> <value> [-lsb]"));
    return 1;
  }
  for (int p : {dataPin, clockPin, latchPin}) if (!checkPin(p, io)) return 1;
  const bool lsb = (strstr(argv[argc - 1], "lsb") != nullptr);

  pinMode(dataPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(latchPin, OUTPUT);
  digitalWrite(latchPin, LOW);
  shiftOut(dataPin, clockPin, lsb ? LSBFIRST : MSBFIRST, (uint8_t)value);
  digitalWrite(latchPin, HIGH);      // latch makes the byte appear at once

  io.out.printf("shifted 0x%02X (%s first)\n", value & 0xFF, lsb ? "LSB" : "MSB");
  return 0;
}

// ---- rgb : the on-board addressable LED --------------------------------------
static int cmd_rgb(int argc, char **argv, ShellIO &io) {
  if (argc < 2) {
    io.out.println(F("usage: rgb <r> <g> <b> [-p pin]   |   rgb off"));
    io.out.println(F("drives one WS2812/NeoPixel (0-255 each)"));
    return 1;
  }
  const int pin = intFlag(argc, argv, "-p", ESPE_RGB_LED_PIN);
  if (!checkPin(pin, io)) return 1;

  if (strcmp(argv[1], "off") == 0) {
    rgbLedWrite(pin, 0, 0, 0);
    io.out.println(F("rgb: off"));
    return 0;
  }
  const int r = constrain(argInt(argc, argv, 0, 0), 0, 255);
  const int g = constrain(argInt(argc, argv, 1, 0), 0, 255);
  const int b = constrain(argInt(argc, argv, 2, 0), 0, 255);
  rgbLedWrite(pin, r, g, b);
  io.out.printf("rgb: #%02X%02X%02X on GPIO%d\n", r, g, b, pin);
  return 0;
}

// ---- spi ----------------------------------------------------------------------
static int cmd_spi(int argc, char **argv, ShellIO &io) {
  if (argc < 2) {
    io.out.println(F("usage: spi xfer <cs-pin> <hex-bytes...> [-hz N] [-mode N]"));
    io.out.println(F("   eg: spi xfer 5 9F 00 00 00        (read a flash chip's JEDEC id)"));
    return 1;
  }
  if (strcmp(argv[1], "xfer") != 0) { io.out.println(F("spi: only 'xfer' is supported")); return 1; }
  if (argc < 4) { io.out.println(F("spi: need a CS pin and at least one byte")); return 1; }

  const int cs = atoi(argv[2]);
  if (!checkPin(cs, io)) return 1;
  const int hz = intFlag(argc, argv, "-hz", 1000000);
  const int mode = constrain(intFlag(argc, argv, "-mode", 0), 0, 3);

  std::vector<uint8_t> tx;
  for (int i = 3; i < argc; ++i) {
    if (argv[i][0] == '-') { i++; continue; }        // skip a flag and its value
    tx.push_back((uint8_t)strtol(argv[i], nullptr, 16));
  }
  if (tx.empty()) { io.out.println(F("spi: no bytes to send")); return 1; }

  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  SPI.begin();
  SPI.beginTransaction(SPISettings(hz, MSBFIRST, mode));
  digitalWrite(cs, LOW);
  std::vector<uint8_t> rx;
  rx.reserve(tx.size());
  for (uint8_t b : tx) rx.push_back(SPI.transfer(b));
  digitalWrite(cs, HIGH);
  SPI.endTransaction();

  io.out.print(F("tx:"));
  for (uint8_t b : tx) io.out.printf(" %02X", b);
  io.out.println();
  io.out.print(F("rx:"));
  for (uint8_t b : rx) io.out.printf(" %02X", b);
  io.out.println();
  return 0;
}

const Command SENSOR_CMDS[] = {
  {"dht",      cmd_dht,      "dht <pin> [-11|-22]",          "DHT11/DHT22 temperature + humidity", G_ESP},
  {"sonar",    cmd_sonar,    "sonar <trig> <echo> [-n N]",   "HC-SR04 ultrasonic distance",        G_ESP},
  {"pulse",    cmd_pulse,    "pulse <pin> [-low] [-t ms]",   "measure one pulse width",            G_ESP},
  {"freq",     cmd_freq,     "freq <pin> [-t ms]",           "frequency on a pin (polled)",        G_ESP},
  {"scope",    cmd_scope,    "scope <adc-pin> [-n N] [-d us]","ASCII plot of an analog input",     G_ESP},
  {"logic",    cmd_logic,    "logic <pin>... [-n N] [-d us]","sample pins and draw the waveforms", G_ESP},
  {"shiftout", cmd_shiftout, "shiftout <data> <clk> <latch> <v>", "clock a byte into a 74HC595",   G_ESP},
  {"rgb",      cmd_rgb,      "rgb <r> <g> <b> [-p pin]|off", "drive a WS2812 / NeoPixel",          G_ESP},
  {"spi",      cmd_spi,      "spi xfer <cs> <hex>...",       "SPI transfer on the default bus",    G_ESP},
};
const size_t SENSOR_CMDS_N = sizeof(SENSOR_CMDS) / sizeof(SENSOR_CMDS[0]);
