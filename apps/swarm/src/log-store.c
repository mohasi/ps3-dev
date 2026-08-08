// log-store - the last few hundred log lines, held for the screen to show.

#include "log-store.h"

#include "dbg.h"
#include "string-utilities.h"
#include "thread.h"

static sys_lwmutex_t storeLock;
static char lines[LOG_STORE_MAX][LOG_LINE_MAX];
static int lineCount;    // stops climbing once the ring is full
static int writeIndex;   // where the next line goes
static LogCallback nextCallback;

// the line as dbg.h formatted it, minus the newline the screen has no use for
static void keepLine(const char *line, int length)
{
   int kept = length < LOG_LINE_MAX - 1 ? length : LOG_LINE_MAX - 1;
   while (kept > 0 && (line[kept - 1] == '\n' || line[kept - 1] == '\r')) kept--;

   lock(&storeLock);
   for (int index = 0; index < kept; index++) lines[writeIndex][index] = line[index];
   lines[writeIndex][kept] = 0;

   writeIndex = (writeIndex + 1) % LOG_STORE_MAX;
   if (lineCount < LOG_STORE_MAX) lineCount++;
   unlock(&storeLock);

   if (nextCallback) nextCallback(line, length);
}

void startLogStore(void)
{
   createLock(&storeLock);
   nextCallback = logCallback;   // the bridge installs one of its own, and it still wants the lines
   setLogCallback(keepLine);
}

void stopLogStore(void)
{
   setLogCallback(nextCallback);
   nextCallback = NULL;
   destroyLock(&storeLock);
}

int getLogLineCount(void)
{
   lock(&storeLock);
   int count = lineCount;
   unlock(&storeLock);
   return count;
}

void getLogLine(int index, char *out, int capacity)
{
   out[0] = 0;

   lock(&storeLock);
   if (index >= 0 && index < lineCount) {
      int oldest = lineCount == LOG_STORE_MAX ? writeIndex : 0;
      strCopy(out, capacity, lines[(oldest + index) % LOG_STORE_MAX]);
   }
   unlock(&storeLock);
}
