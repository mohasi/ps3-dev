// Shared FTP server implementation for simple-lib-core.
//
// Anonymous-only, binary-mode FTP server on a configurable port. Supports
// PASV data connections, directory listings (LIST/NLST/MLSD/MLST), file transfer
// (RETR/STOR/APPE), and standard file operations (DELE/MKD/RMD/RNFR/RNTO).
// Best-effort /dev_blind mount on startup.
//
// API: startFtpServer(port) → FtpResult, stopFtpServer(), isFtpServerRunning().
// Listener thread retries socket creation until the network stack is ready.

#include "ftp.h"

#include <arpa/inet.h>
#include <cell/rtc.h>
#include <netinet/in.h>
#include <netex/errno.h>
#include <netex/net.h>
#include <netex/sockinfo.h>
#include <stdint.h>
#include <sys/ppu_thread.h>
#include <sys/socket.h>
#include <sys/sys_time.h>   // sys_time_get_system_time (data-connection pacing)
#include <sys/time.h>
#include <sys/timer.h>

#include "dbg.h"
#include "vfs.h"
#include "string-utilities.h"
#include "syscall.h"   // mountDevBlind
#include "thread.h"

enum {
   FTP_MAX_SESSIONS = 2,
   FTP_BLOCK = 32 * 1024,
   FTP_COMMAND_BUFFER = 1024,
   FTP_PATH_BUFFER = 1024,
   FTP_DIR_BUFFER = 8 * 1024,
   FTP_CTRL_TIMEOUT_S = 600,
   FTP_LOGIN_TIMEOUT_S = 30,         // free a slot fast if a client connects then never speaks
   FTP_DATA_ACCEPT_TIMEOUT_S = 10,   // give up waiting for the client's data connection
   FTP_DATA_XFER_TIMEOUT_S = 30,     // abort a transfer stalled this long (dead or silent peer)
   // PASV listener creation self-paces against the tiny lv2 socket pool: when a rapid run of
   // small-file pulls fills the pool with data sockets lingering in TIME_WAIT, socket()/bind()
   // fail transiently. We retry with a short backoff instead of failing the PASV, so the server
   // throttles to the rate at which TIME_WAIT drains rather than refusing connections. Budget is
   // bounded (~5s) so a genuinely dead stack still gives up and replies 425.
   FTP_PASV_OPEN_BACKOFF_US = 100000,
   FTP_PASV_OPEN_RETRIES = 50,
   // Token-bucket pacing for new data connections (see throttleDataOpen). A measured PS3 holds
   // ~460 sockets and a data socket lingers ~60s (2*MSL) in TIME_WAIT after each transfer, so the
   // sustainable rate is ~7/s. BURST runs at full speed, then the rate settles to REFILL/s, chosen
   // so concurrent TIME_WAIT (~BURST + REFILL*2*MSL) stays well under the pool with headroom for
   // the control connection. Bursty/light loads never spend the burst and see zero added latency.
   FTP_THROTTLE_REFILL_PER_SEC = 5,
   FTP_THROTTLE_BURST = 64,
};

// lv2 mount (syscall 837) returns EINVAL when the target is already mounted.
static const uint32_t MOUNT_ALREADY_MOUNTED = 0x80010002;

// MLSx fact selection (OPTS MLST). Every fact is on by default; OPTS MLST narrows
// the set, and the MLSD/MLST output must then return ONLY the chosen facts
// (RFC 3659 §7.1: facts not requested MUST NOT be returned).
enum {
   MLST_FACT_TYPE     = 1 << 0,
   MLST_FACT_SIZE     = 1 << 1,
   MLST_FACT_MODIFY   = 1 << 2,
   MLST_FACT_UNIX_MODE = 1 << 3,
   MLST_FACT_UNIX_UID = 1 << 4,
   MLST_FACT_UNIX_GID = 1 << 5,
};
#define MLST_FACTS_DEFAULT (MLST_FACT_TYPE | MLST_FACT_SIZE | MLST_FACT_MODIFY | \
   MLST_FACT_UNIX_MODE | MLST_FACT_UNIX_UID | MLST_FACT_UNIX_GID)

typedef struct {
   int controlSocket;
   int pasvSocket;
   int dataSocket;
   uint32_t localIp;
   char currentPath[FTP_PATH_BUFFER];
   char renameFromPath[FTP_PATH_BUFFER];
   char commandBuffer[FTP_COMMAND_BUFFER];
   int commandLength;
   uint64_t restOffset;       // pending REST restart marker; consumed by the next RETR/STOR
   unsigned mlstFacts;        // MLSx facts to emit (OPTS MLST); defaults to MLST_FACTS_DEFAULT
   char ioBuffer[FTP_BLOCK];
   char directoryBuffer[FTP_DIR_BUFFER];
   volatile int alive;
} FtpSession;

// The FTP server is a singleton; all of its live state is private to this file.
typedef struct {
   int listenSocket;
   volatile int stopping;
   volatile int listenerAlive;
   sys_ppu_thread_t listenerThread;
} FtpServer;

// Per-slot lifecycle: FREE → RUNNING (thread live) → NEEDS_JOIN (thread exited,
// resources not yet reclaimed). A NEEDS_JOIN slot is joined and returned to FREE
// either by the listener before reusing it, or by stopFtpServer on teardown.
enum { SLOT_FREE = 0, SLOT_RUNNING = 1, SLOT_NEEDS_JOIN = 2 };

static FtpServer server = { .listenSocket = -1 };
static FtpSession sessionPool[FTP_MAX_SESSIONS];
static volatile int sessionSlotState[FTP_MAX_SESSIONS];
static sys_ppu_thread_t sessionThreadIds[FTP_MAX_SESSIONS];
static sys_lwmutex_t sessionPoolLock;
static volatile int sessionPoolLockReady;
static volatile int listenerSpawned;   // set once the listener thread is created; gates the join in stop

typedef struct {
   const char *name;
   void (*handler)(FtpSession *session, const char *arg);
} FtpCommand;

static void runFtpListenerThread(uint64_t arg);
static void runFtpSessionThread(uint64_t arg);

static void closeSocket(int *socketValue)
{
   if (*socketValue >= 0) {
     shutdown(*socketValue, SHUT_RDWR);
     socketclose(*socketValue);
     *socketValue = -1;
   }
}

// stopFtpServer reads a session's ctrl/data socket fields under sessionPoolLock to
// shut them down from another thread. The owning session thread must mutate those
// same fields under the same lock, otherwise stop can read a stale fd the stack has
// already closed and reused, and shut down an unrelated live socket. These two
// helpers give that serialization; they must NOT be called while already holding
// the lock (the lwmutex is non-recursive).
static void storeSessionSocket(int *field, int value)
{
   lock(&sessionPoolLock);
   *field = value;
   unlock(&sessionPoolLock);
}

static void closeSessionSocket(int *field)
{
   lock(&sessionPoolLock);
   closeSocket(field);
   unlock(&sessionPoolLock);
}

// Returns a bound+listening socket, or -1. On failure, *reason (if non-NULL)
// distinguishes a missing network stack from an already-claimed port.
static int listenOnPort(uint16_t port, FtpResult *reason)
{
   int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
   if (listenSocket < 0) {
     if (reason) *reason = FTP_NETWORK_UNAVAILABLE;
     return -1;
   }

   // Allow rebinding the port while a socket from a just-closed instance lingers
   // in TIME_WAIT (e.g. plugin reload), instead of failing with "port in use".
   int reuse = 1;
   setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

   struct sockaddr_in address;
   memSet(&address, 0, sizeof address);
   address.sin_family = AF_INET;
   address.sin_port = htons(port);
   address.sin_addr.s_addr = htonl(INADDR_ANY);

   if (bind(listenSocket, (struct sockaddr *)&address, sizeof address) < 0) {
     int bindError = sys_net_errno;   // capture before socketclose can clear it
     socketclose(listenSocket);
     // Only a genuine "address already in use" is a real conflict to fail fast on
     // (keeps the app's FTP toggle responsive). Any other bind failure at boot —
     // the network interface still coming up — is transient, so report it as
     // network-not-ready and let the caller keep retrying.
     if (reason) *reason = (bindError == SYS_NET_EADDRINUSE) ? FTP_PORT_IN_USE : FTP_NETWORK_UNAVAILABLE;
     return -1;
   }

   if (listen(listenSocket, FTP_MAX_SESSIONS) < 0) {
     socketclose(listenSocket);
     if (reason) *reason = FTP_NETWORK_UNAVAILABLE;   // not a port conflict; retry through bring-up
     return -1;
   }

   return listenSocket;
}

int isFtpPortAvailable(uint16_t port)
{
   int listenSocket = listenOnPort(port, NULL);
   if (listenSocket < 0) return 0;
   closeSocket(&listenSocket);
   return 1;
}

FtpState isFtpServerRunning(void)
{
   return (server.listenSocket >= 0 || server.listenerAlive) ? FTP_STARTED : FTP_STOPPED;
}

static int sendAll(int socketValue, const char *buffer, int length)
{
   int offset = 0;
   while (offset < length) {
     int sent = send(socketValue, buffer + offset, length - offset, 0);
     if (sent <= 0) return -1;
     offset += sent;
   }
   return 0;
}

static void replyLine(int socketValue, int code, const char *message)
{
   char buffer[512];
   int length = 0;
   buffer[length++] = (char)('0' + (code / 100) % 10);
   buffer[length++] = (char)('0' + (code / 10) % 10);
   buffer[length++] = (char)('0' + code % 10);
   buffer[length++] = ' ';
   for (int index = 0; message[index] && length < (int)sizeof(buffer) - 2; index++) buffer[length++] = message[index];
   buffer[length++] = '\r';
   buffer[length++] = '\n';
   sendAll(socketValue, buffer, length);
}

static void replyQuotedPath(FtpSession *session, int code, const char *path)
{
   char buffer[FTP_PATH_BUFFER + 16];
   int length = 0;
   buffer[length++] = (char)('0' + (code / 100) % 10);
   buffer[length++] = (char)('0' + (code / 10) % 10);
   buffer[length++] = (char)('0' + code % 10);
   buffer[length++] = ' ';
   buffer[length++] = '"';
   for (int index = 0; path[index] && length < (int)sizeof(buffer) - 6; index++) {
     char byte = path[index];
     if (byte == '\r' || byte == '\n') byte = '_';   // never let a name break the control line
     if (byte == '"') buffer[length++] = '"';
     buffer[length++] = byte;
   }
   buffer[length++] = '"';
   buffer[length++] = '\r';
   buffer[length++] = '\n';
   sendAll(session->controlSocket, buffer, length);
}

// Commands that act on a named path reject an empty argument rather than
// silently operating on the current directory. Returns 1 if an arg is present.
static int requirePath(FtpSession *session, const char *arg)
{
   if (arg[0] == 0) {
     replyLine(session->controlSocket, 501, "Path required.");
     return 0;
   }
   return 1;
}

// A mounted-volume (NTFS/exFAT) path longer than the VFS can represent is silently
// truncated by the backend and would hit the WRONG file; reject it. cellFs paths are safe
// up to FTP_PATH_BUFFER, so only the mounted volumes need this guard. Returns 1 if the path fits.
static int pathFitsBackend(FtpSession *session, const char *path)
{
   if (getStrLen(path) >= MAX_PATH_LEN && getScheme(path) != VFS_SCHEME_CELLFS) {
     replyLine(session->controlSocket, 501, "Path too long.");
     return 0;
   }
   return 1;
}

static void resolvePath(FtpSession *session, const char *inputPath, char *outputPath)
{
   int outputLength = 0;
   if (inputPath[0] == '/') {
     // absolute path: copy verbatim
     while (inputPath[outputLength] && outputLength < FTP_PATH_BUFFER - 1) {
       outputPath[outputLength] = inputPath[outputLength];
       outputLength++;
     }
     outputPath[outputLength] = 0;
   } else {
     // relative path: join onto the current directory
     int inputIndex = 0;
     while (session->currentPath[inputIndex] && outputLength < FTP_PATH_BUFFER - 1) {
       outputPath[outputLength++] = session->currentPath[inputIndex++];
     }
     if (outputLength > 0 && outputPath[outputLength - 1] != '/' && outputLength < FTP_PATH_BUFFER - 1)
       outputPath[outputLength++] = '/';
     for (inputIndex = 0; inputPath[inputIndex] && outputLength < FTP_PATH_BUFFER - 1; inputIndex++)
       outputPath[outputLength++] = inputPath[inputIndex];
     outputPath[outputLength] = 0;
   }
   normalizePath(outputPath, FTP_PATH_BUFFER);
}

// Token-bucket pacing for new data connections. Each PASV transfer leaves one socket in TIME_WAIT
// on the PS3 for ~2*MSL after a graceful close, and the lv2 pool holds only ~460 sockets, so pulling
// small files faster than TIME_WAIT drains fills the pool and the stack starts dropping even the
// control connection. We can't skip the TIME_WAIT (a graceful close is required to signal end-of-file
// to the client, and the closer always owns TIME_WAIT) and can't grow the pool, so we cap the rate of
// data-connection creation: a burst of FTP_THROTTLE_BURST runs at full speed, then it settles to
// FTP_THROTTLE_REFILL_PER_SEC/s — keeping concurrent TIME_WAIT (~BURST + REFILL*2*MSL) under the pool
// with headroom for the control connection. Tokens are scaled by ONE_PERMIT so the rate is exact in
// integer math. Bursty/light loads never spend the burst and pay zero latency; only sustained hammering
// throttles, and the server never wedges. State is shared, so it is read/updated under sessionPoolLock;
// the wait sleeps with the lock released.
static int64_t throttleTokens;     // available permits, scaled by ONE_PERMIT
static uint64_t throttleLastUs;    // timestamp of the last refill

static void throttleDataOpen(void)
{
   const int64_t ONE_PERMIT = 1000000;
   for (;;) {
     if (server.stopping) return;

     lock(&sessionPoolLock);
     uint64_t now = sys_time_get_system_time();
     throttleTokens += (int64_t)(now - throttleLastUs) * FTP_THROTTLE_REFILL_PER_SEC;   // elapsed_us * permits/s
     throttleLastUs = now;
     int64_t capacity = (int64_t)FTP_THROTTLE_BURST * ONE_PERMIT;
     if (throttleTokens > capacity) throttleTokens = capacity;
     int spend = (throttleTokens >= ONE_PERMIT);
     if (spend) throttleTokens -= ONE_PERMIT;
     int64_t deficit = ONE_PERMIT - throttleTokens;   // permits still owed before the next open (only used if !spend)
     unlock(&sessionPoolLock);

     if (spend) return;

     // sleep just long enough to accrue the deficit at the refill rate (REFILL permits/s == REFILL
     // scaled-units/us), capped so a stop request is still noticed promptly.
     uint64_t waitUs = (uint64_t)(deficit / FTP_THROTTLE_REFILL_PER_SEC);
     if (waitUs < 1000) waitUs = 1000;
     if (waitUs > 500000) waitUs = 500000;
     sys_timer_usleep((usecond_t)waitUs);
   }
}

// Opens an OS-assigned PASV listen socket, retrying with backoff while the lv2 socket pool is
// momentarily exhausted (see FTP_PASV_OPEN_* above). Returns the socket, or -1 if the pool stays
// dry past the retry budget (or a stop was requested). The retry only sleeps on failure, so a
// pool with headroom returns immediately at full speed; only a saturated pool throttles.
static int openPasv(uint16_t *outPort)
{
   throttleDataOpen();   // pace data-connection creation against the socket pool / TIME_WAIT drain
   if (server.stopping) return -1;

   for (int attempt = 0; ; attempt++) {
     int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
     if (listenSocket >= 0) {
       // let the OS-assigned data port rebind immediately rather than lingering "in use"
       int reuse = 1;
       setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

       struct sockaddr_in address;
       memSet(&address, 0, sizeof address);
       address.sin_family = AF_INET;
       address.sin_port = htons(0);
       address.sin_addr.s_addr = htonl(INADDR_ANY);

       socklen_t addressLength = sizeof address;
       if (bind(listenSocket, (struct sockaddr *)&address, sizeof address) == 0 &&
           listen(listenSocket, 1) == 0 &&
           getsockname(listenSocket, (struct sockaddr *)&address, &addressLength) == 0) {
         *outPort = ntohs(address.sin_port);
         return listenSocket;
       }
       socketclose(listenSocket);   // bind/listen/getsockname failed under pressure — release and retry
     }

     if (server.stopping || attempt >= FTP_PASV_OPEN_RETRIES) return -1;
     sys_timer_usleep(FTP_PASV_OPEN_BACKOFF_US);
   }
}

// Waits for the client to open the data connection, but only up to a deadline:
// a client that issues PASV then never connects must not pin the session thread
// forever (there are only FTP_MAX_SESSIONS slots). Polls in one-second slices so
// a stop request or a dropped control connection is noticed promptly.
static int acceptPasv(FtpSession *session)
{
   if (session->pasvSocket < 0) return -1;

   struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
   setsockopt(session->pasvSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

   int dataSocket = -1;
   for (int waited = 0; waited < FTP_DATA_ACCEPT_TIMEOUT_S; waited++) {
     if (!session->alive || server.stopping) break;
     struct sockaddr_in remoteAddress;
     socklen_t remoteAddressLength = sizeof remoteAddress;
     dataSocket = accept(session->pasvSocket, (struct sockaddr *)&remoteAddress, &remoteAddressLength);
     if (dataSocket >= 0) break;   // connected; otherwise the accept timed out — keep waiting
   }
   closeSocket(&session->pasvSocket);
   return dataSocket;
}

// Bound a stalled data transfer (the peer stops reading / goes silent) so a send can't block
// forever and pin one of the few session slots. Used by every handler that sends on the data
// connection (RETR + the listing commands); STOR sets SO_RCVTIMEO instead since it receives.
static void armDataSendTimeout(int dataSocket)
{
   struct timeval timeout = { .tv_sec = FTP_DATA_XFER_TIMEOUT_S, .tv_usec = 0 };
   setsockopt(dataSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
}

// Days in a given month, accounting for leap years (Gregorian). Used to clamp the
// decoded day so a corrupt mtime can never emit an impossible date like "Feb 31".
static int daysInMonth(int year, int month)
{
   static const int days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
   if (month < 1 || month > 12) return 31;
   if (month == 2) {
     int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
     return leap ? 29 : 28;
   }
   return days[month - 1];
}

// Decomposes a unix timestamp into clamped calendar fields. On RTC failure OR an
// out-of-range year, the whole value falls back to the epoch (1970-01-01 00:00:00) so a
// garbage mtime can never emit a nonsensical date (e.g. 1970 grafted onto a real day).
// The day is clamped to the month's real length so an impossible date is never emitted.
static void decodeTimeClamped(uint64_t modifiedTime,
   int *year, int *month, int *day, int *hour, int *minute, int *second)
{
   CellRtcDateTime dateTime;
   int ok = (cellRtcSetTime_t(&dateTime, modifiedTime) == 0);
   int decodedYear = ok ? (int)dateTime.year : 1970;
   if (decodedYear < 1970 || decodedYear > 9999) { ok = 0; decodedYear = 1970; }

   *year   = decodedYear;
   *month  = ok ? (int)dateTime.month  : 1;   if (*month  < 1 || *month  > 12) *month  = 1;
   *day    = ok ? (int)dateTime.day    : 1;   if (*day < 1 || *day > daysInMonth(*year, *month)) *day = 1;
   *hour   = ok ? (int)dateTime.hour   : 0;   if (*hour   < 0 || *hour   > 23) *hour   = 0;
   *minute = ok ? (int)dateTime.minute : 0;   if (*minute < 0 || *minute > 59) *minute = 0;
   *second = ok ? (int)dateTime.second : 0;   if (*second < 0 || *second > 59) *second = 0;
}

static void formatMlsdTime(uint64_t modifiedTime, char *output)
{
   int year, month, day, hour, minute, second;
   decodeTimeClamped(modifiedTime, &year, &month, &day, &hour, &minute, &second);

   int outputLength = 0;
   output[outputLength++] = (char)('0' + (year / 1000) % 10);
   output[outputLength++] = (char)('0' + (year / 100) % 10);
   output[outputLength++] = (char)('0' + (year / 10) % 10);
   output[outputLength++] = (char)('0' + year % 10);
   output[outputLength++] = (char)('0' + (month / 10) % 10);
   output[outputLength++] = (char)('0' + month % 10);
   output[outputLength++] = (char)('0' + (day / 10) % 10);
   output[outputLength++] = (char)('0' + day % 10);
   output[outputLength++] = (char)('0' + (hour / 10) % 10);
   output[outputLength++] = (char)('0' + hour % 10);
   output[outputLength++] = (char)('0' + (minute / 10) % 10);
   output[outputLength++] = (char)('0' + minute % 10);
   output[outputLength++] = (char)('0' + (second / 10) % 10);
   output[outputLength++] = (char)('0' + second % 10);
}

// Appends src with control bytes (incl. CR/LF) replaced by '_', so a crafted name or path
// can never inject a line break or control sequence into the control channel.
static void appendSanitized(char *buffer, int capacity, int *offset, const char *src)
{
   int outputLength = *offset;
   for (int index = 0; src[index] && outputLength < capacity - 1; index++) {
     unsigned char byte = (unsigned char)src[index];
     buffer[outputLength++] = (byte < 0x20) ? '_' : (char)byte;
   }
   *offset = outputLength;
}

static void appendMlsdLine(char *buffer, int capacity, int *offset, unsigned facts,
   const char *type, uint64_t size, uint32_t mode, uint64_t modifiedTime, const char *name)
{
   int outputLength = *offset;

   // type
   if (facts & MLST_FACT_TYPE) {
     appendStr(buffer, capacity, &outputLength, "type=");
     appendStr(buffer, capacity, &outputLength, type);
     if (outputLength < capacity) buffer[outputLength++] = ';';
   }

   // size — applies only to non-directories (RFC 3659 §7.5.7); omit it for directories.
   int isDirectory = (type[0] == 'd' || type[0] == 'c' || type[0] == 'p');
   if ((facts & MLST_FACT_SIZE) && !isDirectory) {
     appendStr(buffer, capacity, &outputLength, "size=");
     outputLength = appendUint64(buffer, capacity, outputLength, size);
     if (outputLength < capacity) buffer[outputLength++] = ';';
   }

   // modify
   if (facts & MLST_FACT_MODIFY) {
     appendStr(buffer, capacity, &outputLength, "modify=");
     if (outputLength + 14 <= capacity) {
       formatMlsdTime(modifiedTime, buffer + outputLength);
       outputLength += 14;
     }
     if (outputLength < capacity) buffer[outputLength++] = ';';
   }

   // UNIX.mode
   if (facts & MLST_FACT_UNIX_MODE) {
     appendStr(buffer, capacity, &outputLength, "UNIX.mode=0");
     uint32_t permissions = mode & 0777;
     if (outputLength + 3 <= capacity) {
       buffer[outputLength++] = (char)('0' + ((permissions >> 6) & 7));
       buffer[outputLength++] = (char)('0' + ((permissions >> 3) & 7));
       buffer[outputLength++] = (char)('0' + (permissions & 7));
     }
     if (outputLength < capacity) buffer[outputLength++] = ';';
   }

   // UNIX.uid / UNIX.gid
   if (facts & MLST_FACT_UNIX_UID) appendStr(buffer, capacity, &outputLength, "UNIX.uid=nobody;");
   if (facts & MLST_FACT_UNIX_GID) appendStr(buffer, capacity, &outputLength, "UNIX.gid=nobody;");

   // a single space separates the fact set from the name (RFC 3659 §7.2)
   if (outputLength < capacity) buffer[outputLength++] = ' ';

   // name (CR/LF neutralized) + CRLF terminator
   for (int index = 0; name[index] && outputLength < capacity - 2; index++) {
     char byte = name[index];
     if (byte == '\r' || byte == '\n') byte = '_';   // a CR/LF in a name must not end the listing line early
     buffer[outputLength++] = byte;
   }
   if (outputLength + 2 <= capacity) {
     buffer[outputLength++] = '\r';
     buffer[outputLength++] = '\n';
   }

   *offset = outputLength;
}

static int emitMlsdEntry(int dataSocket, char *buffer, int capacity, int *offset, unsigned facts,
   const char *type, const VfsStat *stat, const char *name)
{
   int needed = getStrLen(name) + 128;
   if (*offset > 0 && *offset + needed > capacity) {
     if (sendAll(dataSocket, buffer, *offset) < 0) return -1;
     *offset = 0;
   }

   appendMlsdLine(buffer, capacity, offset, facts, type, stat->size, stat->mode & 0777, stat->mtime, name);
   return 0;
}

static int streamDirMlsd(int dataSocket, const char *path, char *buffer, int capacity, unsigned facts)
{
   int outputLength = 0;
   VfsStat stat;
   if (statPath(path, &stat) == 0) {
     if (emitMlsdEntry(dataSocket, buffer, capacity, &outputLength, facts, "cdir", &stat, ".") < 0) return -1;
   }

   if (!(path[0] == '/' && path[1] == 0)) {
     char parentPath[FTP_PATH_BUFFER];
     getParentPath(path, parentPath, sizeof(parentPath));
     VfsStat parentStat;
     if (statPath(parentPath, &parentStat) == 0) {
       if (emitMlsdEntry(dataSocket, buffer, capacity, &outputLength, facts, "pdir", &parentStat, "..") < 0) return -1;
     }
   }

   VfsDir directoryHandle;
   if (openDir(path, &directoryHandle) != 0) {
     if (outputLength > 0) sendAll(dataSocket, buffer, outputLength);
     return -1;
   }

   char name[256];
   while (readDir(&directoryHandle, name, sizeof name, NULL) == 1) {   // skips "." / ".."
     char fullPath[FTP_PATH_BUFFER];
     joinPath(fullPath, FTP_PATH_BUFFER, path, name);

     VfsStat entryStat;
     if (statPath(fullPath, &entryStat) != 0) continue;

     const char *type = entryStat.isDir ? "dir" : "file";
     if (emitMlsdEntry(dataSocket, buffer, capacity, &outputLength, facts, type, &entryStat, name) < 0) {
       closeDir(&directoryHandle);
       return -1;
     }
   }

   closeDir(&directoryHandle);
   if (outputLength > 0) return sendAll(dataSocket, buffer, outputLength);
   return 0;
}

static const char *const ftpMonthNames[12] = {
   "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

// Builds one `ls -l`-style LIST line: "drwxr-xr-x 1 ftp ftp <size> <Mon> <DD> <HH:MM> <name>".
// Internally bounded by capacity; a CR/LF in the name is neutralized so it can't split the line.
static void appendListLine(char *buffer, int capacity, int *offset, const VfsStat *stat, const char *name)
{
   int outputLength = *offset;

   // permission string (drwxr-xr-x)
   char permissions[11];
   permissions[0] = stat->isDir ? 'd' : '-';
   uint32_t mode = stat->mode & 0777;
   const char *permissionChars = "rwxrwxrwx";
   for (int index = 0; index < 9; index++)
     permissions[1 + index] = (mode & (1u << (8 - index))) ? permissionChars[index] : '-';
   permissions[10] = 0;
   appendStr(buffer, capacity, &outputLength, permissions);
   appendStr(buffer, capacity, &outputLength, " 1 ftp ftp ");

   // size
   outputLength = appendUint64(buffer, capacity, outputLength, stat->size);
   if (outputLength < capacity) buffer[outputLength++] = ' ';

   // date as "Mon DD HH:MM"
   int year, month, day, hour, minute, second;
   decodeTimeClamped(stat->mtime, &year, &month, &day, &hour, &minute, &second);
   appendStr(buffer, capacity, &outputLength, ftpMonthNames[month - 1]);
   if (outputLength + 4 <= capacity) {
     buffer[outputLength++] = ' ';
     buffer[outputLength++] = (char)('0' + day / 10);
     buffer[outputLength++] = (char)('0' + day % 10);
     buffer[outputLength++] = ' ';
   }
   if (outputLength + 6 <= capacity) {
     buffer[outputLength++] = (char)('0' + hour / 10);
     buffer[outputLength++] = (char)('0' + hour % 10);
     buffer[outputLength++] = ':';
     buffer[outputLength++] = (char)('0' + minute / 10);
     buffer[outputLength++] = (char)('0' + minute % 10);
     buffer[outputLength++] = ' ';
   }

   // name (CR/LF neutralized) + CRLF terminator
   for (int index = 0; name[index] && outputLength < capacity - 2; index++) {
     char byte = name[index];
     if (byte == '\r' || byte == '\n') byte = '_';
     buffer[outputLength++] = byte;
   }
   if (outputLength + 2 <= capacity) {
     buffer[outputLength++] = '\r';
     buffer[outputLength++] = '\n';
   }

   *offset = outputLength;
}

// Streams a directory over the data connection in `ls -l` form (LIST). Returns 0 / -1.
static int streamDirList(int dataSocket, const char *path, char *buffer, int capacity)
{
   VfsDir directoryHandle;
   if (openDir(path, &directoryHandle) != 0) return -1;

   int outputLength = 0;
   char name[256];
   while (readDir(&directoryHandle, name, sizeof name, NULL) == 1) {
     char fullPath[FTP_PATH_BUFFER];
     joinPath(fullPath, FTP_PATH_BUFFER, path, name);

     VfsStat entryStat;
     if (statPath(fullPath, &entryStat) != 0) continue;

     int needed = getStrLen(name) + 80;
     if (outputLength > 0 && outputLength + needed > capacity) {
       if (sendAll(dataSocket, buffer, outputLength) < 0) { closeDir(&directoryHandle); return -1; }
       outputLength = 0;
     }
     appendListLine(buffer, capacity, &outputLength, &entryStat, name);
   }

   closeDir(&directoryHandle);
   if (outputLength > 0) return sendAll(dataSocket, buffer, outputLength);
   return 0;
}

// Streams a directory over the data connection as bare names (NLST). Returns 0 / -1.
static int streamDirNlst(int dataSocket, const char *path, char *buffer, int capacity)
{
   VfsDir directoryHandle;
   if (openDir(path, &directoryHandle) != 0) return -1;

   int outputLength = 0;
   char name[256];
   while (readDir(&directoryHandle, name, sizeof name, NULL) == 1) {
     int needed = getStrLen(name) + 2;
     if (outputLength > 0 && outputLength + needed > capacity) {
       if (sendAll(dataSocket, buffer, outputLength) < 0) { closeDir(&directoryHandle); return -1; }
       outputLength = 0;
     }
     appendSanitized(buffer, capacity, &outputLength, name);
     if (outputLength + 2 <= capacity) { buffer[outputLength++] = '\r'; buffer[outputLength++] = '\n'; }
   }

   closeDir(&directoryHandle);
   if (outputLength > 0) return sendAll(dataSocket, buffer, outputLength);
   return 0;
}

// Streams a single object over the data connection (LIST/NLST of a file rather than a directory).
static int streamSingleEntry(int dataSocket, char *buffer, int capacity, int detailed, const VfsStat *stat, const char *name)
{
   int outputLength = 0;
   if (detailed) {
     appendListLine(buffer, capacity, &outputLength, stat, name);
   } else {
     appendSanitized(buffer, capacity, &outputLength, name);
     if (outputLength + 2 <= capacity) { buffer[outputLength++] = '\r'; buffer[outputLength++] = '\n'; }
   }
   return sendAll(dataSocket, buffer, outputLength);
}

static void handleUser(FtpSession *session, const char *arg) { (void)arg; replyLine(session->controlSocket, 331, "User name okay, need password."); }
static void handlePass(FtpSession *session, const char *arg) { (void)arg; replyLine(session->controlSocket, 230, "Anonymous login accepted."); }
static void handleSyst(FtpSession *session, const char *arg) { (void)arg; replyLine(session->controlSocket, 215, "UNIX Type: L8"); }
static void handleNoop(FtpSession *session, const char *arg) { (void)arg; replyLine(session->controlSocket, 200, "OK"); }

// ALLO (pre-allocate) and CLNT (client name) are advisory: we don't need them, but
// reply success so a client that sends them proceeds. Recognized-but-unsupported
// commands (EPSV/EPRT) get 502 ("not implemented"), distinct from 500 ("unrecognized").
static void handleAllo(FtpSession *session, const char *arg) { (void)arg; replyLine(session->controlSocket, 202, "No storage allocation needed."); }
static void handleClnt(FtpSession *session, const char *arg) { (void)arg; replyLine(session->controlSocket, 200, "Noted."); }
static void handleNotImplemented(FtpSession *session, const char *arg) { (void)arg; replyLine(session->controlSocket, 502, "Command not implemented."); }

// REST sets the byte offset for the next RETR/STOR (RFC 3659 §5, REST STREAM). The
// marker is stored and consumed by that transfer; any other command clears it.
static void handleRest(FtpSession *session, const char *arg)
{
   uint64_t offset = 0;
   int sawDigit = 0;
   for (const char *cursor = arg; *cursor; cursor++) {
     if (*cursor < '0' || *cursor > '9') { replyLine(session->controlSocket, 501, "Invalid REST offset."); return; }
     // reject an offset that would overflow uint64 and silently wrap to a wrong, smaller value
     if (offset > (UINT64_MAX - (uint64_t)(*cursor - '0')) / 10) { replyLine(session->controlSocket, 501, "REST offset out of range."); return; }
     offset = offset * 10 + (uint64_t)(*cursor - '0');
     sawDigit = 1;
   }
   if (!sawDigit) { replyLine(session->controlSocket, 501, "Invalid REST offset."); return; }
   session->restOffset = offset;
   replyLine(session->controlSocket, 350, "Restart position accepted; send RETR or STOR.");
}

// Appends one MLST factname to the FEAT list, with a trailing '*' iff that fact is
// currently selected. RFC 3659 §7.8: the asterisk marks the facts MLSx will return,
// and must track the most recent OPTS MLST — so this is driven by session->mlstFacts.
static void appendMlstFeat(char *buffer, int capacity, int *length, const char *name, unsigned facts, unsigned bit)
{
   appendStr(buffer, capacity, length, name);
   if (facts & bit) appendStr(buffer, capacity, length, "*");
   appendStr(buffer, capacity, length, ";");
}

static void handleFeat(FtpSession *session, const char *arg)
{
   (void)arg;
   // Every advertised feature is honored by a real handler. The MLST line stars exactly
   // the facts currently selected for this session (OPTS MLST narrows them), so a FEAT
   // issued after OPTS MLST reflects the active set (RFC 3659 §7.8).
   char buffer[256];
   int length = 0;
   appendStr(buffer, (int)sizeof buffer, &length,
     "211-Features:\r\n"
     " REST STREAM\r\n"
     " SIZE\r\n"
     " MDTM\r\n"
     " MLST ");
   unsigned facts = session->mlstFacts;
   appendMlstFeat(buffer, (int)sizeof buffer, &length, "type",      facts, MLST_FACT_TYPE);
   appendMlstFeat(buffer, (int)sizeof buffer, &length, "size",      facts, MLST_FACT_SIZE);
   appendMlstFeat(buffer, (int)sizeof buffer, &length, "modify",    facts, MLST_FACT_MODIFY);
   appendMlstFeat(buffer, (int)sizeof buffer, &length, "UNIX.mode", facts, MLST_FACT_UNIX_MODE);
   appendMlstFeat(buffer, (int)sizeof buffer, &length, "UNIX.uid",  facts, MLST_FACT_UNIX_UID);
   appendMlstFeat(buffer, (int)sizeof buffer, &length, "UNIX.gid",  facts, MLST_FACT_UNIX_GID);
   appendStr(buffer, (int)sizeof buffer, &length, "\r\n UTF8\r\n211 End.\r\n");
   sendAll(session->controlSocket, buffer, length);
}

// Parses a semicolon-delimited MLST fact list (e.g. "type;size;modify;") into a
// fact bitmask. Fact names are matched case-insensitively; unknown facts are ignored.
static unsigned parseMlstFacts(const char *list)
{
   unsigned facts = 0;
   const char *cursor = list;
   while (*cursor) {
     char token[16];
     int length = 0;
     while (*cursor && *cursor != ';' && length < (int)sizeof(token) - 1) token[length++] = *cursor++;
     token[length] = 0;
     while (*cursor && (*cursor == ';' || *cursor == ' ')) cursor++;   // skip separators
     if (length == 0) continue;

     char upper[16];
     toUpper(upper, (int)sizeof upper, token);
     if      (strEq(upper, "TYPE"))      facts |= MLST_FACT_TYPE;
     else if (strEq(upper, "SIZE"))      facts |= MLST_FACT_SIZE;
     else if (strEq(upper, "MODIFY"))    facts |= MLST_FACT_MODIFY;
     else if (strEq(upper, "UNIX.MODE")) facts |= MLST_FACT_UNIX_MODE;
     else if (strEq(upper, "UNIX.UID"))  facts |= MLST_FACT_UNIX_UID;
     else if (strEq(upper, "UNIX.GID"))  facts |= MLST_FACT_UNIX_GID;
   }
   return facts;
}

// Builds the "MLST OPTS <facts>" echo of the accepted fact set into reply.
static void formatMlstOptsReply(unsigned facts, char *reply, int capacity)
{
   int length = 0;
   appendStr(reply, capacity, &length, "MLST OPTS");
   if (facts) appendStr(reply, capacity, &length, " ");   // SP only when facts follow (RFC 3659 §7.9.1 grammar)
   if (facts & MLST_FACT_TYPE)      appendStr(reply, capacity, &length, "type;");
   if (facts & MLST_FACT_SIZE)      appendStr(reply, capacity, &length, "size;");
   if (facts & MLST_FACT_MODIFY)    appendStr(reply, capacity, &length, "modify;");
   if (facts & MLST_FACT_UNIX_MODE) appendStr(reply, capacity, &length, "UNIX.mode;");
   if (facts & MLST_FACT_UNIX_UID)  appendStr(reply, capacity, &length, "UNIX.uid;");
   if (facts & MLST_FACT_UNIX_GID)  appendStr(reply, capacity, &length, "UNIX.gid;");
   reply[length < capacity ? length : capacity - 1] = 0;
}

// We advertise UTF8 in FEAT, so honor the matching OPTS toggle instead of rejecting
// it; the server is always UTF-8, so any UTF8 setting is accepted. OPTS MLST narrows
// the fact set returned by subsequent MLSD/MLST (RFC 3659 §7.1).
static void handleOpts(FtpSession *session, const char *arg)
{
   char option[8];
   int length = 0;
   while (arg[length] && arg[length] != ' ' && length < (int)sizeof(option) - 1) {
     option[length] = toUpperChar(arg[length]);
     length++;
   }
   option[length] = 0;

   if (strEq(option, "UTF8")) {
     replyLine(session->controlSocket, 200, "UTF8 always on.");
   } else if (strEq(option, "MLST")) {
     // The fact list follows the first space ("MLST type;size;..."). A bare "OPTS MLST"
     // with no list is valid — it clears all facts so subsequent MLSx return names only
     // (RFC 3659 §7.9: "where no factname arguments are present ... only the file names").
     const char *list = arg;
     while (*list && *list != ' ') list++;
     while (*list == ' ') list++;
     session->mlstFacts = parseMlstFacts(list);   // empty list -> 0 facts (names only)
     char reply[96];
     formatMlstOptsReply(session->mlstFacts, reply, (int)sizeof reply);
     replyLine(session->controlSocket, 200, reply);
   } else {
     replyLine(session->controlSocket, 501, "Option not understood.");
   }
}

static void handleQuit(FtpSession *session, const char *arg)
{
   (void)arg;
   replyLine(session->controlSocket, 221, "Goodbye.");
   session->alive = 0;
}

static void handleType(FtpSession *session, const char *arg)
{
   // Always transfer bytes verbatim (binary), but ACCEPT TYPE A as well as I/L: real clients
   // (WinSCP, classic ftp) switch to ASCII before a listing and abort if it's refused. Verbatim
   // transfer is what's wanted for game/save files anyway — ASCII CRLF translation would corrupt
   // binaries. Only a genuinely unknown type is rejected.
   char type = toUpperChar(arg[0]);
   if (type == 'A')                replyLine(session->controlSocket, 200, "Type set to A.");
   else if (type == 'I' || type == 'L') replyLine(session->controlSocket, 200, "Type set to I.");
   else                            replyLine(session->controlSocket, 501, "Unknown TYPE.");
}

static void handleMode(FtpSession *session, const char *arg)
{
   if (toUpperChar(arg[0]) == 'S' && arg[1] == 0) replyLine(session->controlSocket, 200, "Mode set to stream.");
   else replyLine(session->controlSocket, 504, "Only stream mode (MODE S) supported.");
}

static void handleStru(FtpSession *session, const char *arg)
{
   if (toUpperChar(arg[0]) == 'F' && arg[1] == 0) replyLine(session->controlSocket, 200, "Structure set to file.");
   else replyLine(session->controlSocket, 504, "Only file structure (STRU F) supported.");
}

static void handleAbor(FtpSession *session, const char *arg)
{
   (void)arg;
   // The control channel isn't read during a transfer, so any ABOR we see arrives between
   // transfers — there is never one in progress to abort.
   replyLine(session->controlSocket, 226, "No transfer in progress.");
}

static void handleStat(FtpSession *session, const char *arg)
{
   // No argument: general server status (RFC 959 §4.1.3, reply 211).
   if (arg[0] == 0) {
     const char *status =
       "211-simple-ftp status\r\n"
       " Anonymous access, binary mode\r\n"
       "211 End.\r\n";
     sendAll(session->controlSocket, status, getStrLen(status));
     return;
   }

   // STAT <pathname>: a listing sent over the CONTROL connection (RFC 959 §4.1.3 — "analogous
   // to the LIST command except that data shall be transferred over the control connection").
   // Wrapped as a 213 multi-line reply; the ls -l lines start with a permission char, so they
   // can never be mistaken for the "213 " terminator.
   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;

   VfsStat stat;
   if (statPath(targetPath, &stat) != 0) { replyLine(session->controlSocket, 550, "Not found."); return; }

   const char *header = "213-Status:\r\n";
   sendAll(session->controlSocket, header, getStrLen(header));
   if (stat.isDir) {
     streamDirList(session->controlSocket, targetPath, session->directoryBuffer, FTP_DIR_BUFFER);
   } else {
     const char *name = targetPath;
     for (const char *cursor = targetPath; *cursor; cursor++)
       if (*cursor == '/') name = cursor + 1;
     streamSingleEntry(session->controlSocket, session->directoryBuffer, FTP_DIR_BUFFER, 1, &stat, name);
   }
   replyLine(session->controlSocket, 213, "End of status.");
}

static void handlePwd(FtpSession *session, const char *arg)
{
   (void)arg;
   replyQuotedPath(session, 257, session->currentPath);
}

static void handleCwd(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;

   if (strEq(targetPath, "/")) {
     strCopy(session->currentPath, FTP_PATH_BUFFER, targetPath);
     replyLine(session->controlSocket, 250, "Directory changed.");
     return;
   }

   VfsStat stat;

   if (statPath(targetPath, &stat) != 0 || !stat.isDir) {
     replyLine(session->controlSocket, 550, "Directory not found.");
     return;
   }

   strCopy(session->currentPath, FTP_PATH_BUFFER, targetPath);
   replyLine(session->controlSocket, 250, "Directory changed.");
}

static void handleCdup(FtpSession *session, const char *arg)
{
   (void)arg;
   handleCwd(session, "..");
}

static void handlePasv(FtpSession *session, const char *arg)
{
   (void)arg;
   closeSocket(&session->pasvSocket);
   closeSessionSocket(&session->dataSocket);

   // Open PASV listener on OS-assigned port.
   uint16_t port;
   int listenSocket = openPasv(&port);
   if (listenSocket < 0) {
     replyLine(session->controlSocket, 425, "Cannot open passive port.");
     return;
   }
   session->pasvSocket = listenSocket;

   // Build "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" reply.
   const unsigned char *ipBytes = (const unsigned char *)&session->localIp;
   unsigned char portHigh = (unsigned char)(port >> 8);
   unsigned char portLow = (unsigned char)(port & 0xff);

   char line[96];
   int length = 0;
   appendStr(line, (int)sizeof line, &length, "227 Entering Passive Mode (");
   unsigned char parts[6] = { ipBytes[0], ipBytes[1], ipBytes[2], ipBytes[3], portHigh, portLow };
   for (int index = 0; index < 6; index++) {
     unsigned value = parts[index];
     if (value >= 100) {
       line[length++] = (char)('0' + value / 100);
       value %= 100;
       line[length++] = (char)('0' + value / 10);
       line[length++] = (char)('0' + value % 10);
     } else if (value >= 10) {
       line[length++] = (char)('0' + value / 10);
       line[length++] = (char)('0' + value % 10);
     } else {
       line[length++] = (char)('0' + value);
     }
     line[length++] = (index == 5) ? ')' : ',';
   }
   line[length++] = '\r';
   line[length++] = '\n';
   sendAll(session->controlSocket, line, length);
}

static void handleMlsd(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;

   // MLSD lists a directory; reject a file or missing path before opening the data connection.
   VfsStat stat;
   if (statPath(targetPath, &stat) != 0) { replyLine(session->controlSocket, 550, "Directory not found."); return; }
   if (!stat.isDir) { replyLine(session->controlSocket, 501, "Not a directory."); return; }

   storeSessionSocket(&session->dataSocket, acceptPasv(session));
   if (session->dataSocket < 0) {
     replyLine(session->controlSocket, 425, "No data connection.");
     return;
   }
   armDataSendTimeout(session->dataSocket);

   replyLine(session->controlSocket, 150, "Opening data connection.");
   int result = streamDirMlsd(session->dataSocket, targetPath, session->directoryBuffer, FTP_DIR_BUFFER, session->mlstFacts);
   closeSessionSocket(&session->dataSocket);
   replyLine(session->controlSocket, result == 0 ? 226 : 550, result == 0 ? "Transfer complete." : "Directory read failed.");
}

// LIST (detailed `ls -l`) and NLST (bare names) share the data-connection plumbing.
static void handleListing(FtpSession *session, const char *arg, int detailed)
{
   // Some clients send "LIST -la" or "LIST -l /path"; skip leading option flags.
   while (arg[0] == '-') {
     while (*arg && *arg != ' ') arg++;
     while (*arg == ' ') arg++;
   }

   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;

   VfsStat stat;
   if (statPath(targetPath, &stat) != 0) { replyLine(session->controlSocket, 550, "Not found."); return; }

   storeSessionSocket(&session->dataSocket, acceptPasv(session));
   if (session->dataSocket < 0) {
     replyLine(session->controlSocket, 425, "No data connection.");
     return;
   }
   armDataSendTimeout(session->dataSocket);

   replyLine(session->controlSocket, 150, "Opening data connection.");
   int result;
   if (stat.isDir) {
     result = detailed
       ? streamDirList(session->dataSocket, targetPath, session->directoryBuffer, FTP_DIR_BUFFER)
       : streamDirNlst(session->dataSocket, targetPath, session->directoryBuffer, FTP_DIR_BUFFER);
   } else {
     // LIST/NLST on a file lists just that file (RFC 959); name is the trailing path component.
     const char *name = targetPath;
     for (const char *cursor = targetPath; *cursor; cursor++)
       if (*cursor == '/') name = cursor + 1;
     result = streamSingleEntry(session->dataSocket, session->directoryBuffer, FTP_DIR_BUFFER, detailed, &stat, name);
   }
   closeSessionSocket(&session->dataSocket);
   replyLine(session->controlSocket, result == 0 ? 226 : 550, result == 0 ? "Transfer complete." : "Directory read failed.");
}

static void handleList(FtpSession *session, const char *arg) { handleListing(session, arg, 1); }
static void handleNlst(FtpSession *session, const char *arg) { handleListing(session, arg, 0); }

static void handleMlst(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;
   VfsStat stat;
   if (statPath(targetPath, &stat) != 0) {
     replyLine(session->controlSocket, 550, "File not found.");
     return;
   }

   char buffer[2 * FTP_PATH_BUFFER + 256];
   int outputLength = 0;
   appendStr(buffer, (int)sizeof buffer, &outputLength, "250-Listing ");
   appendSanitized(buffer, (int)sizeof buffer, &outputLength, targetPath);
   buffer[outputLength++] = '\r';
   buffer[outputLength++] = '\n';
   buffer[outputLength++] = ' ';

   const char *type = stat.isDir ? "dir" : "file";
   appendMlsdLine(buffer, (int)sizeof buffer, &outputLength, session->mlstFacts, type, stat.size, stat.mode & 0777, stat.mtime, targetPath);
   appendStr(buffer, (int)sizeof buffer, &outputLength, "250 End\r\n");
   sendAll(session->controlSocket, buffer, outputLength);
}

static void handleMdtm(FtpSession *session, const char *arg)
{
   if (!requirePath(session, arg)) return;

   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;
   VfsStat stat;
   if (statPath(targetPath, &stat) != 0 || stat.isDir) {   // MDTM is for regular files (matches SIZE)
     replyLine(session->controlSocket, 550, "Not a regular file.");
     return;
   }

   char line[32];
   int length = 0;
   line[length++] = '2';
   line[length++] = '1';
   line[length++] = '3';
   line[length++] = ' ';
   formatMlsdTime(stat.mtime, line + length);
   length += 14;
   line[length++] = '\r';
   line[length++] = '\n';
   sendAll(session->controlSocket, line, length);
}

static void handleRetr(FtpSession *session, const char *arg)
{
   if (!requirePath(session, arg)) return;

   // Consume any pending REST marker for this transfer (one-shot).
   uint64_t startOffset = session->restOffset;
   session->restOffset = 0;

   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;

   // RETR is for regular files
   VfsStat stat;
   int exists = (statPath(targetPath, &stat) == 0);
   if (exists && stat.isDir) {
     replyLine(session->controlSocket, 550, "Not a regular file.");
     return;
   }

   VfsFile fileHandle;
   if (openFs(targetPath, VFS_O_RDONLY, &fileHandle) != 0) {
     replyLine(session->controlSocket, 550, "Open failed.");
     return;
   }

   // REST past end-of-file is an out-of-range restart: reject it rather than seek to a
   // backend-dependent spot (some clamp to EOF, some go sparse) and ack an empty body.
   if (startOffset > 0 && (!exists || startOffset > stat.size)) {
     closeFs(&fileHandle);
     replyLine(session->controlSocket, 550, "Cannot restart at offset.");
     return;
   }

   // REST: resume from the requested byte offset.
   if (startOffset > 0 && seekFs(&fileHandle, (int64_t)startOffset, VFS_SEEK_SET) < 0) {
     closeFs(&fileHandle);
     replyLine(session->controlSocket, 550, "Cannot restart at offset.");
     return;
   }

   int replyCode = 226;
   const char *replyMessage = "Transfer complete.";

   // open the data connection
   storeSessionSocket(&session->dataSocket, acceptPasv(session));
   if (session->dataSocket < 0) {
     replyCode = 425;
     replyMessage = "No data connection.";
     goto cleanup;
   }
   // no explicit SO_SNDBUF — use the stack default (matches the reference server)
   armDataSendTimeout(session->dataSocket);

   replyLine(session->controlSocket, 150, "Opening data connection.");

   // stream the file
   for (;;) {
     if (server.stopping || !session->alive) { replyCode = 550; replyMessage = "Transfer aborted."; break; }
     int64_t bytesRead = readFs(&fileHandle, session->ioBuffer, FTP_BLOCK);
     if (bytesRead < 0) { replyCode = 550; replyMessage = "Transfer aborted."; break; }
     if (bytesRead == 0) break;
     if (sendAll(session->dataSocket, session->ioBuffer, (int)bytesRead) < 0) { replyCode = 550; replyMessage = "Transfer aborted."; break; }
   }

cleanup:
   closeFs(&fileHandle);
   closeSessionSocket(&session->dataSocket);
   replyLine(session->controlSocket, replyCode, replyMessage);
}

static void handleStorOrAppe(FtpSession *session, const char *arg, int append)
{
   if (!requirePath(session, arg)) return;

   // Consume any pending REST marker for this transfer (one-shot). A resumed STOR must
   // not truncate the existing file; it overwrites/extends from the offset instead.
   uint64_t startOffset = session->restOffset;
   session->restOffset = 0;
   int resume = (!append && startOffset > 0);

   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;

   // STOR/APPE are for regular files; reject a directory target up front.
   VfsStat stat;
   int exists = (statPath(targetPath, &stat) == 0);
   if (exists && stat.isDir) {
     replyLine(session->controlSocket, 550, "Not a regular file.");
     return;
   }

   // A resume must land within the existing file; a marker past EOF (or on a file that
   // isn't there) is out of range — reject it instead of writing at a backend-dependent
   // offset and acking 226 on data the client never placed where it thinks.
   if (resume && (!exists || startOffset > stat.size)) {
     replyLine(session->controlSocket, 550, "Cannot restart at offset.");
     return;
   }

   int openFlags = VFS_O_WRONLY | VFS_O_CREAT;
   if (append)      openFlags |= VFS_O_APPEND;
   else if (!resume) openFlags |= VFS_O_TRUNC;   // a fresh STOR truncates; a resumed one keeps existing bytes
   VfsFile fileHandle;
   if (openFs(targetPath, openFlags, &fileHandle) != 0) {
     replyLine(session->controlSocket, 550, "Open failed.");
     return;
   }

   // REST: resume writing from the requested byte offset.
   if (resume && seekFs(&fileHandle, (int64_t)startOffset, VFS_SEEK_SET) < 0) {
     closeFs(&fileHandle);
     replyLine(session->controlSocket, 550, "Cannot restart at offset.");
     return;
   }

   int replyCode = 226;
   const char *replyMessage = "Transfer complete.";
   int failed = 0;
   int filled = 0;   // bytes buffered in ioBuffer; flushed to disk only as full blocks
   // bound a stalled upload (peer goes silent without closing) so it can't pin the slot forever;
   // no explicit SO_RCVBUF — use the stack default (matches the reference server)
   struct timeval dataTimeout = { .tv_sec = FTP_DATA_XFER_TIMEOUT_S, .tv_usec = 0 };

   storeSessionSocket(&session->dataSocket, acceptPasv(session));
   if (session->dataSocket < 0) {
     replyCode = 425;
     replyMessage = "No data connection.";
     goto cleanup;
   }
   setsockopt(session->dataSocket, SOL_SOCKET, SO_RCVTIMEO, &dataTimeout, sizeof dataTimeout);

   replyLine(session->controlSocket, 150, "Opening data connection.");

   // receive + coalesce small TCP segments into one full-block writeFs (each disk write is a
   // whole FTP_BLOCK, like the reference server's MSG_WAITALL — but without its truncate-on-RST
   // hazard, since we accumulate with plain recv and always flush the trailing partial on close).
   for (;;) {
     if (server.stopping || !session->alive) { failed = 1; break; }
     int received = recv(session->dataSocket, session->ioBuffer + filled, FTP_BLOCK - filled, 0);
     if (received < 0) { failed = 1; break; }
     if (received == 0) {   // peer closed: flush the final partial block
       if (filled > 0 && writeFs(&fileHandle, session->ioBuffer, (uint64_t)filled) != (int64_t)filled) failed = 1;
       break;
     }
     filled += received;
     if (filled == FTP_BLOCK) {
       if (writeFs(&fileHandle, session->ioBuffer, FTP_BLOCK) != FTP_BLOCK) { failed = 1; break; }
       filled = 0;
     }
   }
   if (failed) { replyCode = 550; replyMessage = "Transfer aborted."; }

cleanup:
   // flush + verify the commit before acking: a failed final commit (e.g. an exFAT directory-entry
   // write) must not ack 226 on a corrupt upload. fsyncFs forces it, and closeFs now also reports a
   // commit error deferred to close - escalate either to 550 while we can still answer.
   if (replyCode == 226 && fsyncFs(&fileHandle) != 0) { replyCode = 550; replyMessage = "Transfer aborted."; }
   if (closeFs(&fileHandle) != 0 && replyCode == 226) { replyCode = 550; replyMessage = "Transfer aborted."; }
   syncPath(targetPath);   // force the upload to stable storage before acknowledging it
   closeSessionSocket(&session->dataSocket);
   replyLine(session->controlSocket, replyCode, replyMessage);
}

static void handleStor(FtpSession *session, const char *arg) { handleStorOrAppe(session, arg, 0); }
static void handleAppe(FtpSession *session, const char *arg) { handleStorOrAppe(session, arg, 1); }

static void handleSize(FtpSession *session, const char *arg)
{
   if (!requirePath(session, arg)) return;

   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;
   VfsStat stat;
   if (statPath(targetPath, &stat) != 0 || stat.isDir) {   // SIZE is only defined for regular files
     replyLine(session->controlSocket, 550, "Not a regular file.");
     return;
   }

   char line[64];
   int length = 0;
   line[length++] = '2';
   line[length++] = '1';
   line[length++] = '3';
   line[length++] = ' ';
   length = appendUint64(line, (int)sizeof line, length, stat.size);
   line[length++] = '\r';
   line[length++] = '\n';
   sendAll(session->controlSocket, line, length);
}

static void handleDele(FtpSession *session, const char *arg)
{
   if (!requirePath(session, arg)) return;

   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;
   VfsStat stat;
   if (statPath(targetPath, &stat) == 0 && !stat.isDir && removeFilePath(targetPath) == 0) {
     syncPath(targetPath);
     replyLine(session->controlSocket, 250, "File deleted.");
   } else {
     replyLine(session->controlSocket, 550, "Delete failed.");
   }
}

static void handleMkd(FtpSession *session, const char *arg)
{
   if (!requirePath(session, arg)) return;

   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;
   VfsStat stat;
   if (statPath(targetPath, &stat) == 0 || makeDirPath(targetPath) != 0) {   // fail if it already exists
     replyLine(session->controlSocket, 550, "Mkdir failed.");
     return;
   }
   syncPath(targetPath);
   replyQuotedPath(session, 257, targetPath);
}

static void handleRmd(FtpSession *session, const char *arg)
{
   if (!requirePath(session, arg)) return;

   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) return;
   if (removeDirPath(targetPath) == 0) {
     syncPath(targetPath);
     replyLine(session->controlSocket, 250, "Directory removed.");
   } else {
     replyLine(session->controlSocket, 550, "Rmdir failed.");
   }
}

static void handleRnfr(FtpSession *session, const char *arg)
{
   if (!requirePath(session, arg)) return;

   resolvePath(session, arg, session->renameFromPath);
   if (!pathFitsBackend(session, session->renameFromPath)) { session->renameFromPath[0] = 0; return; }
   VfsStat stat;
   if (statPath(session->renameFromPath, &stat) != 0) {
     session->renameFromPath[0] = 0;
     replyLine(session->controlSocket, 550, "File not found.");
     return;
   }
   replyLine(session->controlSocket, 350, "Ready for RNTO.");
}

static void handleRnto(FtpSession *session, const char *arg)
{
   if (session->renameFromPath[0] == 0) {
     replyLine(session->controlSocket, 503, "Send RNFR first.");
     return;
   }
   if (!requirePath(session, arg)) { session->renameFromPath[0] = 0; return; }

   char targetPath[FTP_PATH_BUFFER];
   resolvePath(session, arg, targetPath);
   if (!pathFitsBackend(session, targetPath)) { session->renameFromPath[0] = 0; return; }
   if (renamePath(session->renameFromPath, targetPath) == 0) {
     syncPath(targetPath);
     replyLine(session->controlSocket, 250, "Rename complete.");
   } else {
     replyLine(session->controlSocket, 550, "Rename failed.");
   }
   session->renameFromPath[0] = 0;
}

static const FtpCommand ftpCommands[] = {
   { "USER", handleUser }, { "PASS", handlePass },
   { "SYST", handleSyst }, { "FEAT", handleFeat }, { "OPTS", handleOpts },
   { "NOOP", handleNoop }, { "QUIT", handleQuit },
   { "TYPE", handleType }, { "MODE", handleMode }, { "STRU", handleStru },
   { "PWD", handlePwd }, { "XPWD", handlePwd },
   { "CWD", handleCwd }, { "CDUP", handleCdup },
   { "PASV", handlePasv },
   { "LIST", handleList }, { "NLST", handleNlst },
   { "MLSD", handleMlsd }, { "MLST", handleMlst }, { "MDTM", handleMdtm },
   { "RETR", handleRetr }, { "REST", handleRest },
   { "STOR", handleStor }, { "APPE", handleAppe },
   { "SIZE", handleSize },
   { "DELE", handleDele },
   { "MKD", handleMkd }, { "XMKD", handleMkd },
   { "RMD", handleRmd }, { "XRMD", handleRmd },
   { "RNFR", handleRnfr }, { "RNTO", handleRnto },
   { "ABOR", handleAbor }, { "STAT", handleStat },
   { "ALLO", handleAllo }, { "CLNT", handleClnt },
   { "EPSV", handleNotImplemented }, { "EPRT", handleNotImplemented },
   // Recognized base-FTP commands we don't implement: reply 502 (not the 500 an unknown verb
   // gets) so a client can tell "unimplemented" from "unrecognized" (RFC 959 §4.2). PORT in
   // particular tells the client "no active mode — use PASV"; active mode is intentionally
   // omitted (an anonymous server with PORT is an FTP-bounce/port-scan relay).
   { "PORT", handleNotImplemented }, { "ACCT", handleNotImplemented },
   { "SMNT", handleNotImplemented }, { "REIN", handleNotImplemented },
   { "HELP", handleNotImplemented }, { "STOU", handleNotImplemented },
};

static void dispatchCommand(FtpSession *session, char *line)
{
   char *space = line;
   while (*space && *space != ' ') space++;

   char verb[8];
   int verbLength = (int)(space - line);
   if (verbLength > 7) verbLength = 7;
   for (int index = 0; index < verbLength; index++) {
     verb[index] = toUpperChar(line[index]);
   }
   verb[verbLength] = 0;

   const char *arg = (*space == ' ') ? space + 1 : "";
   int commandCount = (int)(sizeof(ftpCommands) / sizeof(ftpCommands[0]));
   for (int index = 0; index < commandCount; index++) {
     if (strEq(verb, ftpCommands[index].name)) {
       // A pending RNFR is consumed only by RNTO; any other command invalidates it, so a
       // stale RNFR target can't be renamed by a later unrelated RNTO.
       if (!strEq(verb, "RNFR") && !strEq(verb, "RNTO")) session->renameFromPath[0] = 0;
       // A pending REST marker is consumed only by the next RETR/STOR; APPE always appends
       // at end-of-file and ignores it, and any other command clears it so a stale offset
       // can't apply to an unrelated transfer.
       if (!strEq(verb, "REST") && !strEq(verb, "RETR") && !strEq(verb, "STOR"))
         session->restOffset = 0;
       ftpCommands[index].handler(session, arg);
       return;
     }
   }

   replyLine(session->controlSocket, 500, "Command unrecognized.");
}

static void runFtpSession(FtpSession *session)
{
   // Learn local IP from the control socket for PASV replies.
   session->localIp = 0;   // reset before query so a reused slot can't keep a stale IP
   sys_net_sockinfo_t socketInfo;
   if (sys_net_get_sockinfo(session->controlSocket, &socketInfo, 1) >= 0) {
     session->localIp = (uint32_t)socketInfo.local_adr.s_addr;
   }

   // Start with a short timeout so a client that connects then never speaks frees its slot
   // quickly (only FTP_MAX_SESSIONS slots); the long idle window is granted once it speaks.
   struct timeval timeout = { .tv_sec = FTP_LOGIN_TIMEOUT_S, .tv_usec = 0 };
   setsockopt(session->controlSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
   int idleTimeoutRaised = 0;

   // Send welcome banner.
   replyLine(session->controlSocket, 220, "simple-ftp ready — anonymous access, binary mode.");

   // Initialize session state. ctrl/pasv/data sockets were already set by the
   // listener under the pool lock at slot acquisition; don't re-write dataSocket
   // here (stopFtpServer reads it under the lock, this would be an unlocked write).
   session->alive = 1;
   session->commandLength = 0;
   strCopy(session->currentPath, FTP_PATH_BUFFER, "/");
   session->renameFromPath[0] = 0;
   session->restOffset = 0;
   session->mlstFacts = MLST_FACTS_DEFAULT;

   // Command recv/parse/dispatch loop.
   while (session->alive && !server.stopping) {
     int space = FTP_COMMAND_BUFFER - session->commandLength - 1;
     if (space <= 0) {
       session->commandLength = 0;
       space = FTP_COMMAND_BUFFER - 1;
     }

     int received = recv(session->controlSocket, session->commandBuffer + session->commandLength, space, 0);
     if (received <= 0) break;
     if (!idleTimeoutRaised) {   // first traffic from a real client: grant the long idle window
       struct timeval idle = { .tv_sec = FTP_CTRL_TIMEOUT_S, .tv_usec = 0 };
       setsockopt(session->controlSocket, SOL_SOCKET, SO_RCVTIMEO, &idle, sizeof idle);
       idleTimeoutRaised = 1;
     }
     session->commandLength += received;

     // Extract complete CRLF (or LF) lines and dispatch.
     int scan = 0;
     while (scan < session->commandLength) {
       int lineEnd = scan;
       while (lineEnd < session->commandLength && session->commandBuffer[lineEnd] != '\n') lineEnd++;
       if (lineEnd >= session->commandLength) break;

       int commandEnd = lineEnd;
       if (commandEnd > scan && session->commandBuffer[commandEnd - 1] == '\r') commandEnd--;
       session->commandBuffer[commandEnd] = 0;
       dispatchCommand(session, session->commandBuffer + scan);
       scan = lineEnd + 1;
       if (!session->alive || server.stopping) break;
     }

     // Shift remaining partial line to start of buffer.
     if (scan > 0) {
       int remaining = session->commandLength - scan;
       for (int index = 0; index < remaining; index++)
         session->commandBuffer[index] = session->commandBuffer[scan + index];
       session->commandLength = remaining;
     }
   }

   // ctrl/data closed under the pool lock — stopFtpServer may be shutting these
   // same fds down from another thread (see storeSessionSocket/closeSessionSocket).
   closeSessionSocket(&session->dataSocket);
   closeSocket(&session->pasvSocket);
   closeSessionSocket(&session->controlSocket);
}

static void runFtpSessionThread(uint64_t arg)
{
   FtpSession *session = (FtpSession *)(uintptr_t)arg;
   int index = (int)(session - sessionPool);

   runFtpSession(session);

   // Hand the slot back as "needs join": the listener (before reuse) or
   // stopFtpServer joins this thread, so its resources are always reclaimed.
   lock(&sessionPoolLock);
   sessionSlotState[index] = SLOT_NEEDS_JOIN;
   unlock(&sessionPoolLock);
   exitThread();
}

static void runFtpListenerThread(uint64_t arg)
{
   (void)arg;
   server.listenerAlive = 1;

   // Accept loop: spawn a session thread for each connection.
   while (!server.stopping) {
     struct sockaddr_in remoteAddress;
     socklen_t remoteAddressLength = sizeof remoteAddress;
     int controlSocket = accept(server.listenSocket, (struct sockaddr *)&remoteAddress, &remoteAddressLength);
     if (controlSocket < 0) {
       if (server.stopping) break;
       sys_timer_usleep(100000);
       continue;
     }

     lock(&sessionPoolLock);

     // Reap any finished session threads so their slots become reusable.
     for (int index = 0; index < FTP_MAX_SESSIONS; index++) {
       if (sessionSlotState[index] == SLOT_NEEDS_JOIN) {
         joinThread(sessionThreadIds[index]);
         sessionSlotState[index] = SLOT_FREE;
       }
     }

     // Find a free slot for this connection.
     int slot = -1;
     for (int index = 0; index < FTP_MAX_SESSIONS; index++) {
       if (sessionSlotState[index] == SLOT_FREE) {
         slot = index;
         break;
       }
     }
     if (slot < 0) {
       unlock(&sessionPoolLock);
       replyLine(controlSocket, 421, "Too many connections.");
       shutdown(controlSocket, SHUT_RDWR);
       socketclose(controlSocket);
       continue;
     }

     // Wire up the session and start its thread. The thread id is stored before
     // the slot is marked RUNNING so stopFtpServer always has a valid id to join.
     FtpSession *session = &sessionPool[slot];
     session->controlSocket = controlSocket;
     session->pasvSocket = -1;
     session->dataSocket = -1;

     int result = spawnJoinableThread(&sessionThreadIds[slot], runFtpSessionThread, (uint64_t)(uintptr_t)session,
        THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_64KB, "ftp-session");
     if (result != 0) {
       unlock(&sessionPoolLock);
       replyLine(controlSocket, 421, "Server busy.");
       shutdown(controlSocket, SHUT_RDWR);
       socketclose(controlSocket);
       continue;
     }
     sessionSlotState[slot] = SLOT_RUNNING;
     unlock(&sessionPoolLock);
   }

   server.listenerAlive = 0;
   exitThread();
}

FtpResult startFtpServer(uint16_t port)
{
   if (server.listenSocket >= 0 || server.listenerAlive) return FTP_ALREADY_RUNNING;

   // Init fields individually — never write 0 to listenSocket (a memSet would make
   // it transiently fd 0, which isFtpServerRunning would read as "started").
   server.listenSocket = -1;
   server.stopping = 0;
   server.listenerAlive = 0;
   listenerSpawned = 0;
   memSet(sessionPool, 0, sizeof(sessionPool));
   memSet((void *)sessionSlotState, 0, sizeof(sessionSlotState));

   // Start the data-connection pacer with a full burst available (see throttleDataOpen).
   throttleTokens = (int64_t)FTP_THROTTLE_BURST * 1000000;
   throttleLastUs = sys_time_get_system_time();

   // Create the session-pool lock before any session thread can run.
   if (createLock(&sessionPoolLock) != 0) {
     logError("[ftp] pool lock create failed\n");
     return FTP_THREAD_CREATE_FAILED;
   }
   sessionPoolLockReady = 1;

   // Best-effort mount /dev_blind before listening. The mount is idempotent:
   // 0x80010002 (EINVAL) just means it was already mounted (e.g. the host app
   // mounted it at startup), which is fine — only flag other failures.
   int64_t mountResult = mountDevBlind();
   if (mountResult == 0) logInfo("[ftp] mount /dev_blind ok\n");
   else if ((uint32_t)mountResult == MOUNT_ALREADY_MOUNTED) logInfo("[ftp] /dev_blind already mounted\n");
   else logError("[ftp] mount /dev_blind rc 0x%x\n", (int)mountResult);

   // Create the listen socket, retrying only while the network stack is still
   // coming up. A port already in use won't free itself, so fail fast on that.
   FtpResult reason = FTP_NETWORK_UNAVAILABLE;
   int listenSocket = -1;
   for (int retries = 0; retries < 5; retries++) {
     listenSocket = listenOnPort(port, &reason);
     if (listenSocket >= 0 || reason != FTP_NETWORK_UNAVAILABLE) break;
     logWarn("[ftp] network not ready, retrying\n");
     sys_timer_sleep(2);
   }
   if (listenSocket < 0) {
     logError("[ftp] giving up — port %d unavailable\n", (int)port);
     destroyLock(&sessionPoolLock);
     sessionPoolLockReady = 0;
     return reason;
   }
   server.listenSocket = listenSocket;
   logInfo("[ftp] listening on :%d\n", (int)port);

   // Accept loop runs on its own thread.
   int result = spawnJoinableThread(&server.listenerThread, runFtpListenerThread, 0,
      THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_8KB, "ftp-listener");
   if (result != 0) {
     logError("[ftp] listener thread create failed rc=0x%x\n", result);
     closeSocket(&server.listenSocket);
     destroyLock(&sessionPoolLock);
     sessionPoolLockReady = 0;
     return FTP_THREAD_CREATE_FAILED;
   }
   listenerSpawned = 1;

   return FTP_OK;
}

void stopFtpServer(void)
{
   // Signal stop and tear down the listener. Join it unconditionally if it was
   // ever spawned: its tid is valid even before it sets listenerAlive, so this
   // can't skip a listener that is still about to spawn a session thread (which
   // would then escape the session-join loop below).
   server.stopping = 1;
   closeSocket(&server.listenSocket);
   if (listenerSpawned) {
     joinThread(server.listenerThread);
     listenerSpawned = 0;
   }

   if (sessionPoolLockReady) {
     // Wake every live session so its blocking recv/send returns. Only shut the
     // sockets down here (never close) — the owning thread does the actual close
     // under the same lock, so there is no cross-thread close/shutdown of a
     // possibly-reused fd. pasvSocket is not touched: acceptPasv polls in 1s
     // slices and exits on server.stopping on its own.
     lock(&sessionPoolLock);
     for (int index = 0; index < FTP_MAX_SESSIONS; index++) {
       if (sessionSlotState[index] != SLOT_RUNNING) continue;
       sessionPool[index].alive = 0;
       if (sessionPool[index].controlSocket >= 0) shutdown(sessionPool[index].controlSocket, SHUT_RDWR);
       if (sessionPool[index].dataSocket >= 0) shutdown(sessionPool[index].dataSocket, SHUT_RDWR);
     }
     unlock(&sessionPoolLock);

     // Join every session thread so none is still executing when we return
     // (the caller may unload this prx immediately after stop).
     for (int index = 0; index < FTP_MAX_SESSIONS; index++) {
       lock(&sessionPoolLock);
       int state = sessionSlotState[index];
       sys_ppu_thread_t threadId = sessionThreadIds[index];
       unlock(&sessionPoolLock);
       if (state == SLOT_RUNNING || state == SLOT_NEEDS_JOIN) {
         joinThread(threadId);
         lock(&sessionPoolLock);
         sessionSlotState[index] = SLOT_FREE;
         unlock(&sessionPoolLock);
       }
     }

     destroyLock(&sessionPoolLock);
     sessionPoolLockReady = 0;
   }

   // Clear the running indicators. listenSocket is already -1 (closed above); we
   // never write 0 to it, so a concurrent isFtpServerRunning can't see fd 0 as
   // "running". startFtpServer zeroes the whole struct again on the next start.
   server.stopping = 0;
   server.listenerAlive = 0;
}
