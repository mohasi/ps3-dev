#pragma once

// video-export - hands a finished download to the system so it shows up in the XMB's Video column.
// Copying a file into /dev_hdd0/video is not enough: the XMB lists videos from its own media database,
// and a file that isn't in there is invisible however often the XMB is restarted. cellVideoExport
// registers the file and moves it to wherever the system keeps videos, which is the same thing the
// "copy from USB" route does.
//
// The calls are asynchronous and report back through the app's sysutil callback pump, so requests are
// queued here and stepped one at a time from the main loop.

void initVideoExport(void);      // load the export module; call once at startup
void shutdownVideoExport(void);  // call at exit

// queue a finished file for export. path is the full path, title is what the XMB will show.
// safe to call from the download worker.
void queueVideoExport(const char *path, const char *title);

void updateVideoExport(void);    // step the export along; call once a frame, after appPoll

// where downloads must be written for the export to accept them. valid after initVideoExport.
const char *getVideoStagingDir(void);
