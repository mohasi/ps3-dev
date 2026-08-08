// dbg - the process-wide log sink pointer declared in dbg.h. lives in a real
// translation unit so every file that logs shares one callback, instead of
// each translation unit getting its own dead copy.
#include "dbg.h"

LogCallback logCallback = 0;
int logDetailed = 0;   // off until an app asks for it, so an ordinary log is short and says little
