// torrent-service - the tunnel and the downloads, kept on one background thread.
//
// Everything the library owns is touched by that thread only, except through the lock below. The
// screen calls in, the lock is held for a few instructions, and drawing carries on.

#include "torrent-service.h"

#include <sys/sys_time.h>   // sys_time_get_system_time, for timing a delete

#include "settings.h"
#include "blocked-words.h"
#include "dbg.h"
#include "string-utilities.h"
#include "thread.h"
#include "torrent-net.h"
#include "tunnel-https.h"
#include "tunnel-torrent.h"
#include "http.h"
#include "wg-config.h"
#include "wg-net.h"
#include "network.h"
#include "direct-https.h"
#include "direct-torrent.h"

#define TAG "[swarm] "

#define PIECE_MAX       16777216   // one piece, and large torrents use 16 MB ones
#define DESCRIPTION_MAX  1048576   // a torrent's own description, when it came from a magnet link
#define MAGNET_MAX           512
#define ADD_QUEUE_MAX          8
#define TUNNEL_WAIT_MS     20000
#define SERVICE_STEP_MS       10
#define SCRUB_MAX          65536

static sys_lwmutex_t serviceLock;
static sys_ppu_thread_t serviceThread;
static volatile int serviceRunning;
static volatile int stopRequested;
static int usingTunnel;   // 1 through the vpn, 0 over the console's own connection
static volatile ServiceStatus status = SERVICE_CONNECTING;
static char message[64] = "connecting to the vpn";
static char address[16];
static char configPath[128];

// what the screen has asked for and the service thread has not picked up yet
static char waitingMagnets[ADD_QUEUE_MAX][MAGNET_MAX];
static int  waitingMagnetCount;
static int8_t pauseRequests[TORRENT_SLOT_MAX];    // 1 pause, -1 resume, 0 nothing asked
static int8_t removeRequests[TORRENT_SLOT_MAX];   // 1 when the screen asked for it to go

// what the screen reads: written by the service thread after each pass, never by the engine itself
static ServiceTorrent published[TORRENT_SLOT_MAX];
static int publishedCount;

// the search, and what it found. found[] is the service thread's own, results[] is the screen's copy.
static TorrentSource sources[SOURCE_MAX];
static int sourceCount;
static TorrentItem found[SERVICE_RESULT_MAX];
static int8_t foundSource[SERVICE_RESULT_MAX];
static int foundCount;

static ServiceResult results[SERVICE_RESULT_MAX];
static int resultCount;
static char waitingQuery[SOURCE_QUERY_MAX];
static volatile int searchWanted;
static volatile int searching;
static int8_t addRequests[SERVICE_RESULT_MAX];

static uint8_t pieceBuffer[PIECE_MAX];
static uint8_t scrubBuffer[SCRUB_MAX];   // what a secure delete writes over the content with
static uint8_t descriptions[ADD_QUEUE_MAX][DESCRIPTION_MAX];

// what each torrent was added from, in slot order, so the list can be written out and read back
static char torrentLinks[TORRENT_SLOT_MAX][MAGNET_MAX];
static int8_t descriptionOfTorrent[TORRENT_SLOT_MAX];   // the buffer holding it, -1 for none
static int torrentLinkCount;
static int chosenDescription = -1;                      // the buffer the last add took

static void setMessage(ServiceStatus newStatus, const char *text)
{
   lock(&serviceLock);
   status = newStatus;
   strCopy(message, sizeof message, text);
   unlock(&serviceLock);
}

static void runSearch(void);
static void addFoundTorrent(int index);
static int  addLink(const char *link);
static void rememberTorrent(const char *link);
static void forgetTorrent(int slot);

// take what the screen asked for and act on it, on this thread
static void takeRequests(void)
{
   char magnets[ADD_QUEUE_MAX][MAGNET_MAX];
   int8_t pauses[TORRENT_SLOT_MAX], removals[TORRENT_SLOT_MAX];
   int magnetCount = 0;

   lock(&serviceLock);
   magnetCount = waitingMagnetCount;
   for (int index = 0; index < magnetCount; index++) strCopy(magnets[index], MAGNET_MAX, waitingMagnets[index]);
   waitingMagnetCount = 0;

   for (int slot = 0; slot < TORRENT_SLOT_MAX; slot++) {
      pauses[slot] = pauseRequests[slot];
      removals[slot] = removeRequests[slot];
      pauseRequests[slot] = 0;
      removeRequests[slot] = 0;
   }
   unlock(&serviceLock);

   for (int index = 0; index < magnetCount; index++)
      if (addLink(magnets[index]) >= 0) rememberTorrent(magnets[index]);

   for (int slot = 0; slot < TORRENT_SLOT_MAX; slot++) {
      if (pauses[slot] > 0) pauseTorrent(slot);
      else if (pauses[slot] < 0) resumeTorrent(slot);
   }

   // removals go last and from the end, so the earlier slots keep their numbers while we work
   for (int slot = TORRENT_SLOT_MAX - 1; slot >= 0; slot--) {
      if (!removals[slot]) continue;

      // the path has to be read before the slot goes, and deleted after: the engine holds it open
      char content[MAX_PATH_LEN];
      int havePath = removals[slot] > 1 ? getTorrentContentPath(slot, content, sizeof content) : -1;

      removeTorrent(slot);
      forgetTorrent(slot);

      if (havePath < 0) continue;

      uint64_t freed = 0;
      uint64_t startedMs = sys_time_get_system_time() / 1000;
      int gone = isSecureDeleteOn() ? shredTree(content, &freed, scrubBuffer, sizeof scrubBuffer)
                                    : deleteTree(content, &freed);

      logTrace(TAG "delete: %s, %lld KB, %d ms, %s\n", content, (long long)(freed / 1024),
              (int)(sys_time_get_system_time() / 1000 - startedMs),
              gone == 0 ? "gone from the disk" : "could not be deleted");
   }

   // section: what the search screen asked for
   int8_t adds[SERVICE_RESULT_MAX];
   lock(&serviceLock);
   for (int index = 0; index < SERVICE_RESULT_MAX; index++) {
      adds[index] = addRequests[index];
      addRequests[index] = 0;
   }
   unlock(&serviceLock);

   for (int index = 0; index < foundCount; index++)
      if (adds[index]) addFoundTorrent(index);

   if (searchWanted) {
      searchWanted = 0;
      searching = 1;
      runSearch();
      searching = 0;
   }
}

// section: searching, all of it on this thread

// one result, as the screen wants to read it
static void describeResult(ServiceResult *result, const TorrentItem *torrent, const TorrentSource *source)
{
   memSet(result, 0, sizeof *result);
   strCopy(result->title, SERVICE_TITLE_MAX, torrent->title);
   strCopy(result->size, sizeof result->size, torrent->size);
   strCopy(result->sourceName, SOURCE_NAME_MAX, source->name);
   result->seeders = torrent->seeders;
   result->leechers = torrent->leechers;
}

// where one word of a title starts and ends: anything that is not a letter or a digit separates them
static int isWordCharacter(char character)
{
   return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9');
}

// is any word of this title one of the blocked ones? whole words, so "topics" is not "pics"
static int isBlocked(const char *title)
{
   for (int start = 0; title[start];) {
      if (!isWordCharacter(title[start])) { start++; continue; }

      int length = 0;
      while (isWordCharacter(title[start + length])) length++;

      for (int index = 0; index < BLOCKED_WORD_COUNT; index++) {
         const char *word = BLOCKED_WORDS[index];
         int at = 0;
         while (at < length && word[at] && toLowerChar(title[start + at]) == word[at]) at++;
         if (at == length && !word[at]) return 1;
      }

      start += length;
   }

   return 0;
}

// Both tests, not one or the other: a site that sorts its own torrents still files pornography in
// the wrong place, and those are exactly the ones whose titles say what they are.
static int isAdultResult(const TorrentSource *source, const TorrentItem *torrent)
{
   if (source->adultFrom > 0 && torrent->category >= source->adultFrom && torrent->category <= source->adultTo)
      return 1;

   return isBlocked(torrent->title);
}

static void searchOneSource(int sourceIndex, const char *query)
{
   const TorrentSource *source = &sources[sourceIndex];

   char url[SOURCE_URL_MAX + SOURCE_QUERY_MAX * 3];
   if (buildSearchUrl(source, query, url, sizeof url) != 0) return;

   static TorrentItem page[SERVICE_RESULT_MAX];
   int count = loadTorrentResults(source, url, page, SERVICE_RESULT_MAX);

   for (int index = 0; index < count && foundCount < SERVICE_RESULT_MAX; index++) {
      if (!isAdultAllowed() && isAdultResult(source, &page[index])) continue;

      found[foundCount] = page[index];
      foundSource[foundCount] = (int8_t)sourceIndex;
      foundCount++;
   }
}

static void runSearch(void)
{
   char query[SOURCE_QUERY_MAX];

   lock(&serviceLock);
   strCopy(query, sizeof query, waitingQuery);
   unlock(&serviceLock);

   foundCount = 0;
   for (int index = 0; index < sourceCount; index++)
      if (sources[index].enabled) searchOneSource(index, query);

   ServiceResult copies[SERVICE_RESULT_MAX];
   for (int index = 0; index < foundCount; index++)
      describeResult(&copies[index], &found[index], &sources[foundSource[index]]);

   lock(&serviceLock);
   for (int index = 0; index < foundCount; index++) results[index] = copies[index];
   resultCount = foundCount;
   unlock(&serviceLock);

   logTrace(TAG "search: %d results for %s\n", foundCount, query[0] ? query : "the latest");
}

// section: what to take up again after a restart
//
// One file per torrent, holding the link it was added from, in a folder inside downloads. A list of
// everything in one file would say what this console has downloaded long after a torrent was deleted;
// a file per torrent goes when that torrent goes, and deleting the downloads folder takes the lot.
// The name is taken from the link rather than from the torrent, so the folder itself names nothing.

static void getResumeFolder(char *out, int capacity)
{
   joinPath(out, capacity, getDownloadsPath(), "resume");
}

static void getResumePath(const char *link, char *out, int capacity)
{
   static const char DIGITS[] = "0123456789abcdef";

   // any spread of the link's bytes will do: this only has to give one torrent one file name
   uint32_t mixed = 2166136261u;
   for (const char *character = link; *character; character++) mixed = (mixed ^ (uint8_t)*character) * 16777619u;

   char name[16];
   for (int digit = 0; digit < 8; digit++) name[digit] = DIGITS[(mixed >> (28 - digit * 4)) & 0xF];
   strCopy(name + 8, sizeof name - 8, ".txt");

   char folder[MAX_PATH_LEN];
   getResumeFolder(folder, sizeof folder);
   joinPath(out, capacity, folder, name);
}

static void rememberTorrent(const char *link)
{
   if (torrentLinkCount >= TORRENT_SLOT_MAX) return;

   descriptionOfTorrent[torrentLinkCount] = (int8_t)chosenDescription;
   strCopy(torrentLinks[torrentLinkCount++], MAGNET_MAX, link);

   char folder[MAX_PATH_LEN], path[MAX_PATH_LEN];
   getResumeFolder(folder, sizeof folder);
   makeDirPath(folder);
   getResumePath(link, path, sizeof path);

   if (writeFile(path, link, (uint64_t)getStrLen(link)) != 0)
      logError(TAG "resume: what to take up again could not be written\n");
}

static void forgetTorrent(int slot)
{
   if (slot < 0 || slot >= torrentLinkCount) return;

   // the link goes with the torrent, and goes for good when the content did
   char path[MAX_PATH_LEN];
   getResumePath(torrentLinks[slot], path, sizeof path);

   if (isSecureDeleteOn()) shredTree(path, NULL, scrubBuffer, sizeof scrubBuffer);
   else deleteFile(path);

   for (int index = slot; index + 1 < torrentLinkCount; index++) {
      strCopy(torrentLinks[index], MAGNET_MAX, torrentLinks[index + 1]);
      descriptionOfTorrent[index] = descriptionOfTorrent[index + 1];
   }

   torrentLinkCount--;
}

// A description buffer no torrent is holding. Only a magnet keeps one, since its description arrives
// later from a peer; a torrent file is read into one and copied out before the add returns.
static int findFreeDescription(void)
{
   for (int buffer = 0; buffer < ADD_QUEUE_MAX; buffer++) {
      int taken = 0;
      for (int index = 0; index < torrentLinkCount && !taken; index++) taken = descriptionOfTorrent[index] == buffer;
      if (!taken) return buffer;
   }

   return -1;
}

// a magnet link goes to the engine as it is; anything else is an address to fetch the file from
static int addLink(const char *link)
{
   int buffer = findFreeDescription();
   if (buffer < 0) return -1;

   if (startsWith(link, "magnet:")) {
      chosenDescription = buffer;
      return addMagnet(link, descriptions[buffer], DESCRIPTION_MAX);
   }

   static TorrentMeta meta;   // its file list makes it far too large for the stack
   int length = 0;
   chosenDescription = -1;
   if (loadTorrentFile(link, descriptions[buffer], DESCRIPTION_MAX, &length, &meta) != 0) return -1;

   return addTorrent(&meta);
}

// what was downloading when the app was last closed, added again. Pieces already on disk are found
// by the engine itself, so a torrent picks up where it stopped.
static void reloadTorrents(void)
{
   char folder[MAX_PATH_LEN];
   getResumeFolder(folder, sizeof folder);

   VfsDir directory;
   if (openDir(folder, &directory) != 0) return;

   char name[256];
   while (readDir(&directory, name, sizeof name, NULL) == 1) {
      if (name[0] == '.') continue;

      char path[MAX_PATH_LEN], link[MAGNET_MAX];
      joinPath(path, sizeof path, folder, name);
      if (readFile(path, link, sizeof link) <= 0) continue;

      if (addLink(link) < 0) continue;

      descriptionOfTorrent[torrentLinkCount] = (int8_t)chosenDescription;
      strCopy(torrentLinks[torrentLinkCount++], MAGNET_MAX, link);
   }

   closeDir(&directory);
   if (torrentLinkCount > 0) logInfo(TAG "resume: %d torrents taken up again\n", torrentLinkCount);
}

// a hash and the source's own trackers make a magnet, and the description then comes from a peer
static void buildMagnetLink(const TorrentSource *source, const TorrentItem *torrent, char *out, int capacity)
{
   char hashText[SHA1_TEXT_LENGTH];
   int offset = 0;

   formatSha1(hashText, torrent->infoHash);
   appendStr(out, capacity, &offset, "magnet:?xt=urn:btih:");
   appendStr(out, capacity, &offset, hashText);
   appendStr(out, capacity, &offset, "&dn=");
   appendUrlEnc(out, capacity, &offset, torrent->title);

   for (int tracker = 0; tracker < source->trackerCount; tracker++) {
      appendStr(out, capacity, &offset, "&tr=");
      appendUrlEnc(out, capacity, &offset, source->trackers[tracker]);
   }

   out[offset] = 0;
}

// one search result, added as a magnet when the source gives hash and trackers, otherwise fetched
static void addFoundTorrent(int index)
{
   const TorrentSource *source = &sources[foundSource[index]];
   const TorrentItem *torrent = &found[index];

   if (findFreeDescription() < 0) {
      logTrace(TAG "add: %s not added, the list is full\n", torrent->title);
      return;
   }

   char link[MAGNET_MAX];
   const uint8_t *infoHash = torrent->hasInfoHash ? torrent->infoHash : NULL;

   if (torrent->hasInfoHash && source->trackerCount > 0) buildMagnetLink(source, torrent, link, sizeof link);
   else if (buildTorrentUrl(source, infoHash, torrent->torrentUrl, link, sizeof link) != 0) link[0] = 0;

   if (!link[0] || addLink(link) < 0) {
      logTrace(TAG "add: %s could not be added, hash %d, trackers %d, link %s\n", torrent->title,
               torrent->hasInfoHash, source->trackerCount, torrent->torrentUrl[0] ? torrent->torrentUrl : "none");
      return;
   }

   rememberTorrent(link);
   logTrace(TAG "add: %s\n", torrent->title);
}

// hand the screen a copy of where every torrent has got to
static void publishProgress(void)
{
   ServiceTorrent copies[TORRENT_SLOT_MAX];
   int count = getTorrentCount();

   for (int slot = 0; slot < count; slot++) {
      TorrentProgress progress;
      getTorrentProgress(slot, &progress);

      memSet(&copies[slot], 0, sizeof copies[slot]);
      copies[slot].status = progress.status;
      copies[slot].piecesDone = progress.piecesDone;
      copies[slot].pieceCount = progress.pieceCount;
      copies[slot].bytesDone = progress.bytesDone;
      copies[slot].totalLength = progress.totalLength;
      copies[slot].peerCount = progress.peerCount;
      copies[slot].seederCount = progress.seederCount;
      copies[slot].checking = progress.checking;
      copies[slot].connectedCount = progress.connectedCount;
      copies[slot].bytesPerSecond = progress.bytesPerSecond;
      strCopy(copies[slot].name, TORRENT_NAME_MAX, progress.name ? progress.name : "");
      strCopy(copies[slot].reason, TORRENT_REASON_MAX, progress.reason ? progress.reason : "");
   }

   lock(&serviceLock);
   for (int slot = 0; slot < count; slot++) published[slot] = copies[slot];
   publishedCount = count;
   unlock(&serviceLock);
}

// Bring the tunnel up, if it is wanted. Returns 1 when everything will go through it, 0 when the
// console's own connection is to be used instead, and -1 when nothing may go out at all.
static int chooseNetwork(void)
{
   if (!isVpnEnabled()) return 0;

   int mayFallBack = !isKillSwitchOn();

   if (startWgNetwork(configPath) != 0) {
      if (!mayFallBack) setMessage(SERVICE_FAILED, "the vpn config could not be read");
      return mayFallBack ? 0 : -1;
   }

   for (int waited = 0; waited < TUNNEL_WAIT_MS && !isWgNetworkReady() && !stopRequested; waited += SERVICE_STEP_MS)
      if (serviceWgNetwork(SERVICE_STEP_MS) != 0) break;

   if (isWgNetworkReady()) return 1;

   stopWgNetwork();
   if (!mayFallBack) setMessage(SERVICE_FAILED, "the vpn did not answer");
   return mayFallBack ? 0 : -1;
}

static void runService(uint64_t argument)
{
   (void)argument;

   // section: which way out, and whether there is one at all
   usingTunnel = chooseNetwork();
   if (usingTunnel < 0) {
      serviceRunning = 0;
      exitThread();
   }

   if (usingTunnel) {
      useTunnelForHttps();
      useTunnelForTorrents();
   } else {
      useConsoleForHttps();
      useConsoleForTorrents();
   }

   if (startTorrentEngine(getDownloadsPath(), pieceBuffer, sizeof pieceBuffer) != 0) {
      setMessage(SERVICE_FAILED, "the downloads could not be started");
      if (usingTunnel) stopWgNetwork();
      serviceRunning = 0;
      exitThread();
   }

   int shippedCount = 0;
   const SourceFile *shipped = getShippedSources(&shippedCount);
   sourceCount = loadTorrentSources(getSourcesPath(), sources, SOURCE_MAX, shipped, shippedCount);
   reloadTorrents();

   WgConfig config;
   if (usingTunnel && loadWgConfig(&config, configPath) == 0) formatIpv4(address, sizeof address, config.tunnelAddress);
   else if (!usingTunnel) {
      uint32_t local = 0;
      if (getLocalIpv4(&local) == 0) formatIpv4(address, sizeof address, local);
   }

   setMessage(SERVICE_READY, usingTunnel ? "connected" : "connected without the vpn");

   while (!stopRequested) {
      serviceWgNetwork(SERVICE_STEP_MS);
      takeRequests();
      serviceTorrentEngine();
      publishProgress();
   }

   // section: putting it down
   stopTorrentEngine();
   shutdownHttp();
   stopWgNetwork();
   serviceRunning = 0;
   exitThread();
}

int startTorrentService(const char *path)
{
   if (serviceRunning) return 0;

   strCopy(configPath, sizeof configPath, path);
   if (createLock(&serviceLock) != 0) return -1;

   stopRequested = 0;
   serviceRunning = 1;

   // above the drawing thread: a frame late is nothing, a packet late costs a peer
   if (spawnThread(&serviceThread, runService, 0, THREAD_PRIORITY_HIGH, THREAD_STACK_SIZE_64KB, "swarm-net") != 0) {
      serviceRunning = 0;
      destroyLock(&serviceLock);
      logError(TAG "service: the download thread could not be started\n");
      return -1;
   }

   return 0;
}

void stopTorrentService(void)
{
   if (!serviceRunning) return;

   stopRequested = 1;
   while (serviceRunning) sleepMs(SERVICE_STEP_MS);

   destroyLock(&serviceLock);
}

ServiceStatus getServiceStatus(void)
{
   return status;
}

const char *getServiceMessage(void)
{
   return message;
}

const char *getServiceAddress(void)
{
   return address;
}

// the sites the search reads, as "one, two, three". they are loaded once at startup and never change
void getServiceSourceNames(char *out, int capacity)
{
   int offset = 0;

   for (int index = 0; index < sourceCount; index++) {
      if (!sources[index].enabled) continue;
      if (offset > 0) appendStr(out, capacity, &offset, ", ");
      appendStr(out, capacity, &offset, sources[index].name);
   }

   out[offset] = 0;
}

int isServiceUsingTunnel(void)
{
   return usingTunnel > 0;
}

int getServiceTorrentCount(void)
{
   lock(&serviceLock);
   int count = publishedCount;
   unlock(&serviceLock);
   return count;
}

void getServiceTorrent(int slot, ServiceTorrent *torrent)
{
   memSet(torrent, 0, sizeof *torrent);
   if (slot < 0 || slot >= TORRENT_SLOT_MAX) return;

   lock(&serviceLock);
   *torrent = published[slot];
   unlock(&serviceLock);
}

int addServiceMagnet(const char *magnetUri)
{
   lock(&serviceLock);
   int room = waitingMagnetCount < ADD_QUEUE_MAX;
   if (room) strCopy(waitingMagnets[waitingMagnetCount++], MAGNET_MAX, magnetUri);
   unlock(&serviceLock);

   return room ? 0 : -1;
}

void pauseServiceTorrent(int slot)
{
   if (slot < 0 || slot >= TORRENT_SLOT_MAX) return;

   lock(&serviceLock);
   pauseRequests[slot] = 1;
   unlock(&serviceLock);
}

void resumeServiceTorrent(int slot)
{
   if (slot < 0 || slot >= TORRENT_SLOT_MAX) return;

   lock(&serviceLock);
   pauseRequests[slot] = -1;
   unlock(&serviceLock);
}

void searchServiceSources(const char *query)
{
   lock(&serviceLock);
   strCopy(waitingQuery, sizeof waitingQuery, query);
   resultCount = 0;
   unlock(&serviceLock);

   searchWanted = 1;
}

int isServiceSearching(void)
{
   return searching || searchWanted;
}

int getServiceResultCount(void)
{
   lock(&serviceLock);
   int count = resultCount;
   unlock(&serviceLock);
   return count;
}

void getServiceResult(int index, ServiceResult *result)
{
   memSet(result, 0, sizeof *result);
   if (index < 0 || index >= SERVICE_RESULT_MAX) return;

   lock(&serviceLock);
   *result = results[index];
   unlock(&serviceLock);
}

int addServiceResult(int index)
{
   if (index < 0 || index >= SERVICE_RESULT_MAX) return -1;

   lock(&serviceLock);
   addRequests[index] = 1;
   unlock(&serviceLock);
   return 0;
}

void removeServiceTorrent(int slot, int deleteContent)
{
   if (slot < 0 || slot >= TORRENT_SLOT_MAX) return;

   lock(&serviceLock);
   removeRequests[slot] = deleteContent ? 2 : 1;
   unlock(&serviceLock);
}
