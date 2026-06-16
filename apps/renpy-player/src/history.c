#include "history.h"

#include <string.h>   // memset

#include "vars.h"     // variable snapshots for rollback

#define HIST_MAX 8192

static Frame history[HIST_MAX];
static int   histLen;   // number of recorded frames
static int   histPos;   // cursor: the frame currently being shown

void resetHistory(void)
{
    for (int i = 0; i < histLen; i++)
        if (history[i].varSnapOwned && history[i].varSnap) freeVarsSnap(history[i].varSnap);
    histLen = 0; histPos = 0;
}

Frame *appendHistory(void)
{
    if (histLen >= HIST_MAX) return NULL;
    Frame *frame = &history[histLen];
    memset(frame, 0, sizeof *frame);
    // Snapshot the variable store for rollback. If nothing changed since the previous frame's
    // snapshot, share it (owned = 0) so memory tracks the number of DISTINCT states, not lines.
    if (histLen > 0 && !varsAreDirty())
    {
        frame->varSnap = history[histLen - 1].varSnap;
        frame->varSnapOwned = 0;
    }
    else
    {
        frame->varSnap = snapshotVars();
        frame->varSnapOwned = 1;
        clearVarsDirty();
    }
    histPos = histLen;
    histLen++;
    return frame;
}

const Frame *getHistoryCurrent(void) { return histLen > 0 ? &history[histPos] : NULL; }
const Frame *getHistoryLatest(void)  { return histLen > 0 ? &history[histLen - 1] : NULL; }

int isHistoryAtLatest(void)    { return histPos >= histLen - 1; }
int stepHistoryBack(void)    { if (histPos > 0) { histPos--; return 1; } return 0; }
int stepHistoryForward(void) { if (histPos < histLen - 1) { histPos++; return 1; } return 0; }

int getHistoryNvlPage(const char **who, const char **what, int max)
{
    int last = histPos;
    int first = last;
    while (first > 0 && history[first - 1].nvl && history[first - 1].nvlPage == history[last].nvlPage) first--;
    if (last - first >= max) first = last - (max - 1);   // older lines scrolled off the page
    int count = 0;
    for (int k = first; k <= last; k++) { who[count] = history[k].who; what[count] = history[k].what; count++; }
    return count;
}
