#include "shell.h"

// ============================================================================
//  Archive / package-manager commands.
//  These need compression libraries / an OS package system that a stock ESP32
//  Arduino build does not provide, so they are honest stubs (listed in help).
// ============================================================================

static int cmd_archive_stub(int argc, char **argv, ShellIO &io) {
  io.out.print(argv[0]);
  io.out.println(F(": compression is not built in on ESP32 (unsupported)"));
  return 1;
}

static int cmd_apt(int argc, char **argv, ShellIO &io) {
  io.out.println(F("apt: no package manager on ESP32."));
  io.out.println(F("     Commands are compiled into the firmware; reflash to add more."));
  return 1;
}

const Command MISC_CMDS[] = {
  {"tar",    cmd_archive_stub, "tar ...",    "(unsupported: no archiver)",      G_ARCHIVE},
  {"gzip",   cmd_archive_stub, "gzip f",     "(unsupported: no zlib)",          G_ARCHIVE},
  {"gunzip", cmd_archive_stub, "gunzip f",   "(unsupported: no zlib)",          G_ARCHIVE},
  {"zip",    cmd_archive_stub, "zip ...",    "(unsupported: no zip)",           G_ARCHIVE},
  {"unzip",  cmd_archive_stub, "unzip f",    "(unsupported: no zip)",           G_ARCHIVE},
  {"apt",    cmd_apt,          "apt ...",    "(no package manager)",            G_ARCHIVE},
};
const size_t MISC_CMDS_N = sizeof(MISC_CMDS) / sizeof(MISC_CMDS[0]);
