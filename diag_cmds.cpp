#include "shell.h"
#include "config.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp_system.h"

// ============================================================================
//  Diagnostics: the "how is this board actually doing" commands.
// ============================================================================

// ---- temp : internal die temperature ---------------------------------------
// The classic ESP32's sensor is undocumented and reads well above ambient
// (it measures the die, next to the radio), so it is useful as a trend, not
// as a thermometer - the output says so rather than pretending otherwise.
static int cmd_temp(int argc, char **argv, ShellIO &io) {
  float c = temperatureRead();
  if (isnan(c)) { io.out.println(F("temp: no reading from the internal sensor")); return 1; }
  io.out.printf("die temperature: %.1f C  (%.1f F)\n", c, c * 9.0 / 5.0 + 32.0);
  io.out.println(F("note: this is the on-die sensor - it runs hot vs. ambient; watch the trend"));
  return 0;
}

const Command DIAG_CMDS[] = {
  {"temp", cmd_temp, "temp", "internal die temperature", G_ESP},
};
const size_t DIAG_CMDS_N = sizeof(DIAG_CMDS) / sizeof(DIAG_CMDS[0]);
