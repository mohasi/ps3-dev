#pragma once

// The downloads, running on their own thread so drawing never waits for the network.
//
// The engine and the tunnel belong to that thread alone. A screen only reads progress and posts
// what the user asked for, both behind one lock, so nothing is half-written when it is read.

#include <stdint.h>

#include "torrent-engine.h"
#include "torrent-feed.h"
#include "torrent-source.h"

typedef enum {
   SERVICE_CONNECTING,
   SERVICE_READY,
   SERVICE_FAILED
} ServiceStatus;

int  startTorrentService(const char *configPath);   // brings the tunnel up in the background
void stopTorrentService(void);

ServiceStatus getServiceStatus(void);
const char   *getServiceMessage(void);   // one line about what it is doing, for the screen
const char   *getServiceAddress(void);   // the address the tunnel gave us, empty until it is up
int           isServiceUsingTunnel(void);   // 0 when traffic is going out over the console's own line
void          getServiceSourceNames(char *out, int capacity);   // the sites being searched, comma separated

// What the screen shows. The service thread writes this out after every pass, so a screen reads a
// copy and never waits on the network or the disk.
typedef struct {
   TorrentStatus status;
   char          name[TORRENT_NAME_MAX];
   char          reason[TORRENT_REASON_MAX];   // why it failed, empty otherwise
   int           piecesDone;
   int           pieceCount;
   int64_t       bytesDone;
   int64_t       totalLength;
   int           peerCount;
   int           seederCount;
   int           checking;
   int           connectedCount;
   int           bytesPerSecond;
} ServiceTorrent;

int  getServiceTorrentCount(void);
void getServiceTorrent(int slot, ServiceTorrent *torrent);

// Searching, which happens on the same thread for the same reason. A screen asks, then watches the
// count until the answers arrive.
#define SERVICE_RESULT_MAX 64
#define SERVICE_TITLE_MAX 160

typedef struct {
   char title[SERVICE_TITLE_MAX];
   char size[24];
   char sourceName[SOURCE_NAME_MAX];
   int  seeders;
   int  leechers;
} ServiceResult;

void searchServiceSources(const char *query);   // empty query browses instead
int  isServiceSearching(void);
int  getServiceResultCount(void);
void getServiceResult(int index, ServiceResult *result);

// add what the search found to the downloads. 0 / -1.
int addServiceResult(int index);

// what the screen asks for. each returns at once; the work happens on the service thread.
int  addServiceMagnet(const char *magnetUri);
void pauseServiceTorrent(int slot);
void resumeServiceTorrent(int slot);
// off the list, and off the disk too when deleteContent says so
void removeServiceTorrent(int slot, int deleteContent);
