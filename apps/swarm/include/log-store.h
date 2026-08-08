#pragma once

// The last few hundred log lines, kept in memory so the screen can show what the app is doing.
//
// Lines arrive from every thread through the same callback dbg.h already writes the file with, so
// whatever was logging before this is chained to and keeps working.

#include "dbg.h"   // LOG_LINE_MAX, the width a line is kept at

#define LOG_STORE_MAX 240

void startLogStore(void);
void stopLogStore(void);

int  getLogLineCount(void);                  // how many are held, oldest first
void getLogLine(int index, char *out, int capacity);
