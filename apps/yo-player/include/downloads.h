#pragma once

// downloads - background video downloads. Square/R3 on a grid queues a video; a single low-priority worker
// drains the queue one at a time, remuxing the same H.264 + AAC streams the player would show into one
// .mp4, then handing it to video-export so it turns up in the XMB's Video column. The active download's
// progress shows top-right on the browse screens. Everything runs off the UI thread; the queue is
// cancelled and cleared on app exit.

#include "extractor.h"   // SearchResult

void initDownloads(void);       // set up the worker state + overlay; call after initFont
void shutdownDownloads(void);   // cancel + clear the queue, join the worker, free the overlay; call at exit

void enqueueDownload(const SearchResult *item);   // add a video to the download queue (spawns the worker if idle)

void updateDownloadOverlay(void);         // refresh the "Downloading ..." label; call each frame in a screen update
void drawDownloadOverlay(int screenWidth); // draw it top-right if a download is active; call in a screen draw
