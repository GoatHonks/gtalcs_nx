/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

#if defined(DEBUG_LOG) && DEBUG_LOG

static int s_nxlinkSock = -1;

static void initNxLink(void) {
  if (R_FAILED(socketInitializeDefault()))
    return;
  s_nxlinkSock = nxlinkStdio();
  if (s_nxlinkSock < 0)
    socketExit();
}

static void deinitNxLink(void) {
  if (s_nxlinkSock >= 0) {
    close(s_nxlinkSock);
    socketExit();
    s_nxlinkSock = -1;
  }
}

void userAppInit(void) {
  initNxLink();
}

void userAppExit(void) {
  deinitNxLink();
}

#endif

// the game's `printf` import points here. With DEBUG_LOG off the body vanishes
// and this is a no-op. Keeps the log file open for the run (reopening per line
// on FAT makes boot take minutes); fflush each line to survive an abrupt exit.
int debugPrintf(char *text, ...) {
#if defined(DEBUG_LOG) && DEBUG_LOG
  va_list list;
  static FILE *f = NULL;
  if (!f)
    f = fopen(LOG_NAME, "a");

  // Milliseconds since the first line, stamped at the start of each line. The
  // log had no clock in it at all, which meant a question as basic as "where
  // does boot time actually go" could not be answered from it -- the frame
  // counter only says how many frames passed, not how long they took.
  static u64 t0 = 0;
  if (!t0)
    t0 = armTicksToNs(armGetSystemTick());
  const unsigned ms =
      (unsigned)((armTicksToNs(armGetSystemTick()) - t0) / 1000000ull);

  // Formatted first so the stamp can be held back mid-line: the game's own
  // printf comes through here too and does not always write whole lines.
  char buf[1024];
  va_start(list, text);
  const int n = vsnprintf(buf, sizeof(buf), text, list);
  va_end(list);
  if (n < 0)
    return 0;

  static int at_line_start = 1;
  if (f) {
    if (at_line_start)
      fprintf(f, "[%6u.%03u] ", ms / 1000, ms % 1000);
    fputs(buf, f);
    fflush(f);
  }
  if (at_line_start)
    printf("[%6u.%03u] ", ms / 1000, ms % 1000);
  fputs(buf, stdout);   // also to nxlink stdout, if a host is connected

  const size_t len = strlen(buf);
  at_line_start = (len && buf[len - 1] == '\n');
#endif
  return 0;
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
