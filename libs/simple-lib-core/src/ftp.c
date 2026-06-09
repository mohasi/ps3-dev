// Shared FTP server implementation for simple-lib-core.
//
// Anonymous-only, binary-mode FTP server on a configurable port. Supports
// PASV data connections, directory listings (MLSD/MLST), file transfer
// (RETR/STOR/APPE), and standard file operations (DELE/MKD/RMD/RNFR/RNTO).
// Best-effort /dev_blind mount on startup.
//
// API: startFtpServer(port) → FtpResult, stopFtpServer(), isFtpServerRunning().
// Listener thread retries socket creation until the network stack is ready.

#include "ftp.h"

#include <arpa/inet.h>
#include <cell/fs/cell_fs_errno.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/rtc.h>
#include <netinet/in.h>
#include <netex/errno.h>
#include <netex/net.h>
#include <netex/sockinfo.h>
#include <stdint.h>
#include <sys/ppu_thread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timer.h>

#include "dbg.h"
#include "file.h"
#include "string-utilities.h"
#include "thread.h"

enum {
   FTP_MAX_SESSIONS = 2,
   FTP_BLOCK = 128 * 1024,
   FTP_CMDBUF = 1024,
   FTP_PATHBUF = 1024,
   FTP_DIRBUF = 8 * 1024,
   FTP_CTRL_TIMEOUT_S = 600,
};

// lv2 mount (syscall 837) returns EINVAL when the target is already mounted.
static const uint32_t MOUNT_ALREADY_MOUNTED = 0x80010002;

typedef struct {
   int ctrlSocket;
   int pasvSocket;
   int dataSocket;
   uint32_t localIp;
   char currentPath[FTP_PATHBUF];
   char renameFromPath[FTP_PATHBUF];
   char commandBuffer[FTP_CMDBUF];
   int commandLength;
   char ioBuffer[FTP_BLOCK];
   char directoryBuffer[FTP_DIRBUF];
   volatile int alive;
} FtpSession;

// The FTP server is a singleton; all of its live state is private to this file.
typedef struct {
   int listenSocket;
   volatile int stopping;
   volatile int listenerAlive;
   sys_ppu_thread_t listenerThread;
} FtpServer;

static FtpServer server = { .listenSocket = -1 };
static FtpSession sessionPool[FTP_MAX_SESSIONS];
static volatile int sessionPoolUsed[FTP_MAX_SESSIONS];

typedef struct {
   const char *name;
   void (*handler)(FtpSession *session, const char *arg);
} FtpCommand;

static void ftpListenerThread(uint64_t arg);
static void ftpSessionThread(uint64_t arg);

static void closeSocket(int *socketValue)
{
   if (*socketValue >= 0) {
      shutdown(*socketValue, SHUT_RDWR);
      socketclose(*socketValue);
      *socketValue = -1;
   }
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

   struct sockaddr_in address;
   memSet(&address, 0, sizeof address);
   address.sin_family = AF_INET;
   address.sin_port = htons(port);
   address.sin_addr.s_addr = htonl(INADDR_ANY);

   if (bind(listenSocket, (struct sockaddr *)&address, sizeof address) < 0) {
      socketclose(listenSocket);
      if (reason) *reason = FTP_PORT_IN_USE;
      return -1;
   }

   if (listen(listenSocket, FTP_MAX_SESSIONS) < 0) {
      socketclose(listenSocket);
      if (reason) *reason = FTP_PORT_IN_USE;
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
   char buffer[FTP_PATHBUF + 16];
   int length = 0;
   buffer[length++] = (char)('0' + (code / 100) % 10);
   buffer[length++] = (char)('0' + (code / 10) % 10);
   buffer[length++] = (char)('0' + code % 10);
   buffer[length++] = ' ';
   buffer[length++] = '"';
   for (int index = 0; path[index] && length < (int)sizeof(buffer) - 6; index++) {
      if (path[index] == '"') buffer[length++] = '"';
      buffer[length++] = path[index];
   }
   buffer[length++] = '"';
   buffer[length++] = '\r';
   buffer[length++] = '\n';
   sendAll(session->ctrlSocket, buffer, length);
}

static FtpSession *acquireSession(void)
{
   for (int index = 0; index < FTP_MAX_SESSIONS; index++) {
      if (!sessionPoolUsed[index]) {
         sessionPoolUsed[index] = 1;
         return &sessionPool[index];
      }
   }
   return NULL;
}

static void releaseSession(FtpSession *session)
{
   int index = (int)(session - sessionPool);
   if (index >= 0 && index < FTP_MAX_SESSIONS) sessionPoolUsed[index] = 0;
}

static void resolvePath(FtpSession *session, const char *inputPath, char *outputPath)
{
   int outputLength = 0;
   if (inputPath[0] == '/') {
      while (inputPath[outputLength] && outputLength < FTP_PATHBUF - 1) {
         outputPath[outputLength] = inputPath[outputLength];
         outputLength++;
      }
      outputPath[outputLength] = 0;
   } else {
      int inputIndex = 0;
      while (session->currentPath[inputIndex] && outputLength < FTP_PATHBUF - 1) {
         outputPath[outputLength++] = session->currentPath[inputIndex++];
      }
      if (outputLength > 0 && outputPath[outputLength - 1] != '/' && outputLength < FTP_PATHBUF - 1)
         outputPath[outputLength++] = '/';
      for (inputIndex = 0; inputPath[inputIndex] && outputLength < FTP_PATHBUF - 1; inputIndex++)
         outputPath[outputLength++] = inputPath[inputIndex];
      outputPath[outputLength] = 0;
   }
   normalizePath(outputPath, FTP_PATHBUF);
}

static int openPasv(uint16_t *outPort)
{
   int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
   if (listenSocket < 0) return -1;

   struct sockaddr_in address;
   memSet(&address, 0, sizeof address);
   address.sin_family = AF_INET;
   address.sin_port = htons(0);
   address.sin_addr.s_addr = htonl(INADDR_ANY);

   if (bind(listenSocket, (struct sockaddr *)&address, sizeof address) < 0) {
      socketclose(listenSocket);
      return -1;
   }

   if (listen(listenSocket, 1) < 0) {
      socketclose(listenSocket);
      return -1;
   }

   socklen_t addressLength = sizeof address;
   if (getsockname(listenSocket, (struct sockaddr *)&address, &addressLength) < 0) {
      socketclose(listenSocket);
      return -1;
   }

   *outPort = ntohs(address.sin_port);
   return listenSocket;
}

static int acceptPasv(FtpSession *session)
{
   if (session->pasvSocket < 0) return -1;
   struct sockaddr_in remoteAddress;
   socklen_t remoteAddressLength = sizeof remoteAddress;
   int dataSocket = accept(session->pasvSocket, (struct sockaddr *)&remoteAddress, &remoteAddressLength);
   closeSocket(&session->pasvSocket);
   return dataSocket;
}

static void formatMlsdTime(uint64_t modifiedTime, char *output)
{
   CellRtcDateTime dateTime;
   if (cellRtcSetTime_t(&dateTime, modifiedTime) != 0) {
      dateTime.year = 1970;
      dateTime.month = 1;
      dateTime.day = 1;
      dateTime.hour = 0;
      dateTime.minute = 0;
      dateTime.second = 0;
   }

   int year = (int)dateTime.year;
   if (year < 1970 || year > 9999) year = 1970;
   int month = (int)dateTime.month;
   if (month < 1 || month > 12) month = 1;
   int day = (int)dateTime.day;
   if (day < 1 || day > 31) day = 1;
   int hour = (int)dateTime.hour;
   if (hour < 0 || hour > 23) hour = 0;
   int minute = (int)dateTime.minute;
   if (minute < 0 || minute > 59) minute = 0;
   int second = (int)dateTime.second;
   if (second < 0 || second > 59) second = 0;

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

static void appendMlsdLine(char *buffer, int capacity, int *offset,
   const char *type, uint64_t size, uint32_t mode, uint64_t modifiedTime, const char *name)
{
   int outputLength = *offset;

   appendStr(buffer, capacity, &outputLength, "type=");
   appendStr(buffer, capacity, &outputLength, type);
   if (outputLength < capacity) buffer[outputLength++] = ';';

   int isDirectory = (type[0] == 'd' || type[0] == 'c' || type[0] == 'p');
   appendStr(buffer, capacity, &outputLength, isDirectory ? "sizd=" : "size=");
   outputLength = appendUint64(buffer, capacity, outputLength, size);
   if (outputLength < capacity) buffer[outputLength++] = ';';

   appendStr(buffer, capacity, &outputLength, "modify=");
   if (outputLength + 14 <= capacity) {
      formatMlsdTime(modifiedTime, buffer + outputLength);
      outputLength += 14;
   }
   if (outputLength < capacity) buffer[outputLength++] = ';';

   appendStr(buffer, capacity, &outputLength, "UNIX.mode=0");
   uint32_t permissions = mode & 0777;
   if (outputLength + 3 <= capacity) {
      buffer[outputLength++] = (char)('0' + ((permissions >> 6) & 7));
      buffer[outputLength++] = (char)('0' + ((permissions >> 3) & 7));
      buffer[outputLength++] = (char)('0' + (permissions & 7));
   }
   if (outputLength < capacity) buffer[outputLength++] = ';';

   appendStr(buffer, capacity, &outputLength, "UNIX.uid=nobody;UNIX.gid=nobody; ");
   for (int index = 0; name[index] && outputLength < capacity - 2; index++) buffer[outputLength++] = name[index];
   if (outputLength + 2 <= capacity) {
      buffer[outputLength++] = '\r';
      buffer[outputLength++] = '\n';
   }

   *offset = outputLength;
}

static int statIsDir(const CellFsStat *stat)
{
   return (stat->st_mode & CELL_FS_S_IFDIR) ? 1 : 0;
}

static int emitMlsdEntry(int dataSocket, char *buffer, int capacity, int *offset,
   const char *type, const CellFsStat *stat, const char *name)
{
   int needed = getStrLen(name) + 128;
   if (*offset > 0 && *offset + needed > capacity) {
      if (sendAll(dataSocket, buffer, *offset) < 0) return -1;
      *offset = 0;
   }

   appendMlsdLine(buffer, capacity, offset, type, stat->st_size, (uint32_t)(stat->st_mode & 0777),
      (uint64_t)stat->st_mtime, name);
   return 0;
}

static int streamDirMlsd(int dataSocket, const char *path, char *buffer, int capacity)
{
   int outputLength = 0;
   CellFsStat stat;
   if (cellFsStat(path, &stat) == CELL_FS_SUCCEEDED) {
      if (emitMlsdEntry(dataSocket, buffer, capacity, &outputLength, "cdir", &stat, ".") < 0) return -1;
   }

   if (!(path[0] == '/' && path[1] == 0)) {
      char parentPath[FTP_PATHBUF];
      getParentPath(path, parentPath, sizeof(parentPath));
      CellFsStat parentStat;
      if (cellFsStat(parentPath, &parentStat) == CELL_FS_SUCCEEDED) {
         if (emitMlsdEntry(dataSocket, buffer, capacity, &outputLength, "pdir", &parentStat, "..") < 0) return -1;
      }
   }

   int directoryHandle;
   if (cellFsOpendir(path, &directoryHandle) != CELL_FS_SUCCEEDED) {
      if (outputLength > 0) sendAll(dataSocket, buffer, outputLength);
      return -1;
   }

   CellFsDirent entry;
   uint64_t entrySize;
   while (cellFsReaddir(directoryHandle, &entry, &entrySize) == CELL_FS_SUCCEEDED && entrySize > 0) {
      if (entry.d_name[0] == '.' && (entry.d_name[1] == 0 || (entry.d_name[1] == '.' && entry.d_name[2] == 0))) continue;

      char fullPath[FTP_PATHBUF];
      joinPath(fullPath, FTP_PATHBUF, path, entry.d_name);

      CellFsStat entryStat;
      if (cellFsStat(fullPath, &entryStat) != CELL_FS_SUCCEEDED) continue;

      const char *type = statIsDir(&entryStat) ? "dir" : "file";
      if (emitMlsdEntry(dataSocket, buffer, capacity, &outputLength, type, &entryStat, entry.d_name) < 0) {
         cellFsClosedir(directoryHandle);
         return -1;
      }
   }

   cellFsClosedir(directoryHandle);
   if (outputLength > 0) return sendAll(dataSocket, buffer, outputLength);
   return 0;
}

static void handleUser(FtpSession *session, const char *arg) { (void)arg; replyLine(session->ctrlSocket, 331, "User name okay, need password."); }
static void handlePass(FtpSession *session, const char *arg) { (void)arg; replyLine(session->ctrlSocket, 230, "Anonymous login accepted."); }
static void handleSyst(FtpSession *session, const char *arg) { (void)arg; replyLine(session->ctrlSocket, 215, "UNIX Type: L8"); }
static void handleNoop(FtpSession *session, const char *arg) { (void)arg; replyLine(session->ctrlSocket, 200, "OK"); }

static void handleFeat(FtpSession *session, const char *arg)
{
   (void)arg;
   const char *features =
      "211-Features:\r\n"
      " SIZE\r\n"
      " MDTM\r\n"
      " MLSD\r\n"
      " MLST type*;size*;sizd*;modify*;UNIX.mode*;UNIX.uid*;UNIX.gid*;\r\n"
      " UTF8\r\n"
      "211 End.\r\n";
   sendAll(session->ctrlSocket, features, getStrLen(features));
}

static void handleQuit(FtpSession *session, const char *arg)
{
   (void)arg;
   replyLine(session->ctrlSocket, 221, "Goodbye.");
   session->alive = 0;
}

static void handleType(FtpSession *session, const char *arg)
{
   (void)arg;
   replyLine(session->ctrlSocket, 200, "Type set to binary.");
}

static void handlePwd(FtpSession *session, const char *arg)
{
   (void)arg;
   replyQuotedPath(session, 257, session->currentPath);
}

static void handleCwd(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);

   if (strEq(targetPath, "/")) {
      strCopy(session->currentPath, FTP_PATHBUF, targetPath);
      replyLine(session->ctrlSocket, 250, "Directory changed.");
      return;
   }

   CellFsStat stat;

   if (cellFsStat(targetPath, &stat) != CELL_FS_SUCCEEDED || !statIsDir(&stat)) {
      replyLine(session->ctrlSocket, 550, "Directory not found.");
      return;
   }

   strCopy(session->currentPath, FTP_PATHBUF, targetPath);
   replyLine(session->ctrlSocket, 250, "Directory changed.");
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
   closeSocket(&session->dataSocket);

   // Open PASV listener on OS-assigned port.
   uint16_t port;
   int listenSocket = openPasv(&port);
   if (listenSocket < 0) {
      replyLine(session->ctrlSocket, 425, "Cannot open passive port.");
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
   sendAll(session->ctrlSocket, line, length);
}

static void handleMlsd(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);
   int dataSocket = acceptPasv(session);
   if (dataSocket < 0) {
      replyLine(session->ctrlSocket, 425, "No data connection.");
      return;
   }

   replyLine(session->ctrlSocket, 150, "Opening data connection.");
   int result = streamDirMlsd(dataSocket, targetPath, session->directoryBuffer, FTP_DIRBUF);
   closeSocket(&dataSocket);
   replyLine(session->ctrlSocket, result == 0 ? 226 : 550, result == 0 ? "Transfer complete." : "Directory read failed.");
}

static void handleMlst(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);
   CellFsStat stat;
   if (cellFsStat(targetPath, &stat) != CELL_FS_SUCCEEDED) {
      replyLine(session->ctrlSocket, 550, "File not found.");
      return;
   }

   char buffer[2 * FTP_PATHBUF + 256];
   int outputLength = 0;
   appendStr(buffer, (int)sizeof buffer, &outputLength, "250-Listing ");
   appendStr(buffer, (int)sizeof buffer, &outputLength, targetPath);
   buffer[outputLength++] = '\r';
   buffer[outputLength++] = '\n';
   buffer[outputLength++] = ' ';

   const char *type = statIsDir(&stat) ? "dir" : "file";
   appendMlsdLine(buffer, (int)sizeof buffer, &outputLength, type, stat.st_size,
      (uint32_t)(stat.st_mode & 0777), (uint64_t)stat.st_mtime, targetPath);
   appendStr(buffer, (int)sizeof buffer, &outputLength, "250 End\r\n");
   sendAll(session->ctrlSocket, buffer, outputLength);
}

static void handleMdtm(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);
   CellFsStat stat;
   if (cellFsStat(targetPath, &stat) != CELL_FS_SUCCEEDED) {
      replyLine(session->ctrlSocket, 550, "File not found.");
      return;
   }

   char line[32];
   int length = 0;
   line[length++] = '2';
   line[length++] = '1';
   line[length++] = '3';
   line[length++] = ' ';
   formatMlsdTime((uint64_t)stat.st_mtime, line + length);
   length += 14;
   line[length++] = '\r';
   line[length++] = '\n';
   sendAll(session->ctrlSocket, line, length);
}

static void handleRetr(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);

   int fileHandle;
   if (cellFsOpen(targetPath, CELL_FS_O_RDONLY, &fileHandle, NULL, 0) != CELL_FS_SUCCEEDED) {
      replyLine(session->ctrlSocket, 550, "Open failed.");
      return;
   }

   int dataSocket = acceptPasv(session);
   if (dataSocket < 0) {
      cellFsClose(fileHandle);
      replyLine(session->ctrlSocket, 425, "No data connection.");
      return;
   }

   int socketBufferSize = FTP_BLOCK;
   setsockopt(dataSocket, SOL_SOCKET, SO_SNDBUF, &socketBufferSize, sizeof socketBufferSize);

   replyLine(session->ctrlSocket, 150, "Opening data connection.");
   int failed = 0;
   for (;;) {
      uint64_t bytesRead = 0;
      if (cellFsRead(fileHandle, session->ioBuffer, FTP_BLOCK, &bytesRead) != CELL_FS_SUCCEEDED) {
         failed = 1;
         break;
      }
      if (bytesRead == 0) break;
      if (sendAll(dataSocket, session->ioBuffer, (int)bytesRead) < 0) {
         failed = 1;
         break;
      }
   }

   cellFsClose(fileHandle);
   closeSocket(&dataSocket);
   replyLine(session->ctrlSocket, failed ? 550 : 226, failed ? "Transfer aborted." : "Transfer complete.");
}

static void handleStorOrAppe(FtpSession *session, const char *arg, int append)
{
   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);

   int openFlags = CELL_FS_O_WRONLY | CELL_FS_O_CREAT | (append ? CELL_FS_O_APPEND : CELL_FS_O_TRUNC);
   int fileHandle;
   if (cellFsOpen(targetPath, openFlags, &fileHandle, NULL, 0) != CELL_FS_SUCCEEDED) {
      replyLine(session->ctrlSocket, 550, "Open failed.");
      return;
   }

   int dataSocket = acceptPasv(session);
   if (dataSocket < 0) {
      cellFsClose(fileHandle);
      replyLine(session->ctrlSocket, 425, "No data connection.");
      return;
   }

   int socketBufferSize = FTP_BLOCK;
   setsockopt(dataSocket, SOL_SOCKET, SO_RCVBUF, &socketBufferSize, sizeof socketBufferSize);

   replyLine(session->ctrlSocket, 150, "Opening data connection.");
   int failed = 0;
   for (;;) {
      int received = recv(dataSocket, session->ioBuffer, FTP_BLOCK, 0);
      if (received < 0) {
         failed = 1;
         break;
      }
      if (received == 0) break;

      uint64_t bytesWritten = 0;
      if (cellFsWrite(fileHandle, session->ioBuffer, (uint64_t)received, &bytesWritten) != CELL_FS_SUCCEEDED ||
         bytesWritten != (uint64_t)received) {
            failed = 1;
            break;
      }
   }

   cellFsClose(fileHandle);
   syncPath(targetPath);
   closeSocket(&dataSocket);
   replyLine(session->ctrlSocket, failed ? 550 : 226, failed ? "Transfer aborted." : "Transfer complete.");
}

static void handleStor(FtpSession *session, const char *arg) { handleStorOrAppe(session, arg, 0); }
static void handleAppe(FtpSession *session, const char *arg) { handleStorOrAppe(session, arg, 1); }

static void handleSize(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);
   CellFsStat stat;
   if (cellFsStat(targetPath, &stat) != CELL_FS_SUCCEEDED) {
      replyLine(session->ctrlSocket, 550, "File not found.");
      return;
   }

   char line[64];
   int length = 0;
   line[length++] = '2';
   line[length++] = '1';
   line[length++] = '3';
   line[length++] = ' ';
   length = appendUint64(line, (int)sizeof line, length, (uint64_t)stat.st_size);
   line[length++] = '\r';
   line[length++] = '\n';
   sendAll(session->ctrlSocket, line, length);
}

static void handleDele(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);
   if (cellFsUnlink(targetPath) == CELL_FS_SUCCEEDED) {
      syncPath(targetPath);
      replyLine(session->ctrlSocket, 250, "File deleted.");
   } else {
      replyLine(session->ctrlSocket, 550, "Delete failed.");
   }
}

static void handleMkd(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);
   if (cellFsMkdir(targetPath, CELL_FS_S_IFDIR | 0777) != CELL_FS_SUCCEEDED) {
      replyLine(session->ctrlSocket, 550, "Mkdir failed.");
      return;
   }
   syncPath(targetPath);
   replyQuotedPath(session, 257, targetPath);
}

static void handleRmd(FtpSession *session, const char *arg)
{
   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);
   if (cellFsRmdir(targetPath) == CELL_FS_SUCCEEDED) {
      syncPath(targetPath);
      replyLine(session->ctrlSocket, 250, "Directory removed.");
   } else {
      replyLine(session->ctrlSocket, 550, "Rmdir failed.");
   }
}

static void handleRnfr(FtpSession *session, const char *arg)
{
   resolvePath(session, arg, session->renameFromPath);
   CellFsStat stat;
   if (cellFsStat(session->renameFromPath, &stat) != CELL_FS_SUCCEEDED) {
      session->renameFromPath[0] = 0;
      replyLine(session->ctrlSocket, 550, "File not found.");
      return;
   }
   replyLine(session->ctrlSocket, 350, "Ready for RNTO.");
}

static void handleRnto(FtpSession *session, const char *arg)
{
   if (session->renameFromPath[0] == 0) {
      replyLine(session->ctrlSocket, 503, "Send RNFR first.");
      return;
   }

   char targetPath[FTP_PATHBUF];
   resolvePath(session, arg, targetPath);
   if (cellFsRename(session->renameFromPath, targetPath) == CELL_FS_SUCCEEDED) {
      syncPath(targetPath);
      replyLine(session->ctrlSocket, 250, "Rename complete.");
   } else {
      replyLine(session->ctrlSocket, 550, "Rename failed.");
   }
   session->renameFromPath[0] = 0;
}

static const FtpCommand ftpCommands[] = {
   { "USER", handleUser }, { "PASS", handlePass },
   { "SYST", handleSyst }, { "FEAT", handleFeat },
   { "NOOP", handleNoop }, { "QUIT", handleQuit },
   { "TYPE", handleType },
   { "PWD", handlePwd }, { "XPWD", handlePwd },
   { "CWD", handleCwd }, { "CDUP", handleCdup },
   { "PASV", handlePasv },
   { "MLSD", handleMlsd }, { "MLST", handleMlst }, { "MDTM", handleMdtm },
   { "RETR", handleRetr },
   { "STOR", handleStor }, { "APPE", handleAppe },
   { "SIZE", handleSize },
   { "DELE", handleDele },
   { "MKD", handleMkd }, { "XMKD", handleMkd },
   { "RMD", handleRmd }, { "XRMD", handleRmd },
   { "RNFR", handleRnfr }, { "RNTO", handleRnto },
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
         ftpCommands[index].handler(session, arg);
         return;
      }
   }

   replyLine(session->ctrlSocket, 502, "Command not implemented.");
}

static void runFtpSession(FtpSession *session)
{
   // Learn local IP from the control socket for PASV replies.
   sys_net_sockinfo_t socketInfo;
   if (sys_net_get_sockinfo(session->ctrlSocket, &socketInfo, 1) >= 0) {
      session->localIp = (uint32_t)socketInfo.local_adr.s_addr;
   }

   // Set idle timeout on control socket.
   struct timeval timeout;
   timeout.tv_sec = FTP_CTRL_TIMEOUT_S;
   timeout.tv_usec = 0;
   setsockopt(session->ctrlSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

   // Send welcome banner.
   replyLine(session->ctrlSocket, 220, "simple-ftp ready — anonymous access, binary mode.");

   // Initialize session state.
   session->alive = 1;
   session->commandLength = 0;
   strCopy(session->currentPath, FTP_PATHBUF, "/");
   session->renameFromPath[0] = 0;
   session->pasvSocket = -1;
   session->dataSocket = -1;

   // Command recv/parse/dispatch loop.
   while (session->alive && !server.stopping) {
      int space = FTP_CMDBUF - session->commandLength - 1;
      if (space <= 0) {
         session->commandLength = 0;
         space = FTP_CMDBUF - 1;
      }

      int received = recv(session->ctrlSocket, session->commandBuffer + session->commandLength, space, 0);
      if (received <= 0) break;
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

   closeSocket(&session->dataSocket);
   closeSocket(&session->pasvSocket);
   closeSocket(&session->ctrlSocket);
}

static void ftpSessionThread(uint64_t arg)
{
   FtpSession *session = (FtpSession *)(uintptr_t)arg;
   runFtpSession(session);
   releaseSession(session);
   exitThread();
}

static void ftpListenerThread(uint64_t arg)
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

      FtpSession *session = acquireSession();
      if (!session) {
         replyLine(controlSocket, 421, "Too many connections.");
         shutdown(controlSocket, SHUT_RDWR);
         socketclose(controlSocket);
         continue;
      }

      session->ctrlSocket = controlSocket;
      session->pasvSocket = -1;
      session->dataSocket = -1;

      sys_ppu_thread_t threadId;
      int result = spawnThread(&threadId, ftpSessionThread, (uint64_t)(uintptr_t)session,
         THREAD_PRIORITY_LOW, THREAD_STACK_SIZE_16KB, "ftp-session");
      if (result != 0) {
         replyLine(controlSocket, 421, "Server busy.");
         shutdown(controlSocket, SHUT_RDWR);
         socketclose(controlSocket);
         releaseSession(session);
      }
   }

   server.listenerAlive = 0;
   exitThread();
}

FtpResult startFtpServer(uint16_t port)
{
   if (server.listenSocket >= 0 || server.listenerAlive) return FTP_ALREADY_RUNNING;

   memSet(&server, 0, sizeof server);
   server.listenSocket = -1;
   memSet(sessionPool, 0, sizeof(sessionPool));
   memSet((void *)sessionPoolUsed, 0, sizeof(sessionPoolUsed));

   // Best-effort mount /dev_blind before listening. The mount is idempotent:
   // 0x80010002 (EINVAL) just means it was already mounted (e.g. the host app
   // mounted it at startup), which is fine — only flag other failures.
   int64_t mountRc = mountDevBlind();
   if (mountRc == 0) logInfo("[ftp] mount /dev_blind ok\n");
   else if ((uint32_t)mountRc == MOUNT_ALREADY_MOUNTED) logInfo("[ftp] /dev_blind already mounted\n");
   else logError("[ftp] mount /dev_blind rc 0x%x\n", (int)mountRc);

   // Create the listen socket, retrying only while the network stack is still
   // coming up. A port already in use won't free itself, so fail fast on that.
   FtpResult reason = FTP_NETWORK_UNAVAILABLE;
   int listenFd = -1;
   for (int retries = 0; retries < 5; retries++) {
      listenFd = listenOnPort(port, &reason);
      if (listenFd >= 0 || reason != FTP_NETWORK_UNAVAILABLE) break;
      logWarn("[ftp] network not ready, retrying\n");
      sys_timer_sleep(2);
   }
   if (listenFd < 0) {
      logError("[ftp] giving up — port %d unavailable\n", (int)port);
      return reason;
   }
   server.listenSocket = listenFd;
   logInfo("[ftp] listening on :%d\n", (int)port);

   // Accept loop runs on its own thread.
   int result = spawnJoinableThread(&server.listenerThread, ftpListenerThread, 0,
      THREAD_PRIORITY_DEFAULT, THREAD_STACK_SIZE_8KB, "ftp-listener");
   if (result != 0) {
      logError("[ftp] listener thread create failed rc=0x%x\n", result);
      closeSocket(&server.listenSocket);
      return FTP_THREAD_CREATE_FAILED;
   }

   return FTP_OK;
}

void stopFtpServer(void)
{
   server.stopping = 1;
   closeSocket(&server.listenSocket);
   if (server.listenerAlive) joinThread(server.listenerThread);

   for (int index = 0; index < FTP_MAX_SESSIONS; index++) {
      if (!sessionPoolUsed[index]) continue;
      sessionPool[index].alive = 0;
      closeSocket(&sessionPool[index].dataSocket);
      closeSocket(&sessionPool[index].pasvSocket);
      closeSocket(&sessionPool[index].ctrlSocket);
   }

   while (1) {
      int activeSessions = 0;
      for (int index = 0; index < FTP_MAX_SESSIONS; index++) {
         if (sessionPoolUsed[index]) {
            activeSessions = 1;
            break;
         }
      }
      if (!activeSessions) break;
      sleepMs(10);
   }

   memSet(&server, 0, sizeof server);
   server.listenSocket = -1;
}
