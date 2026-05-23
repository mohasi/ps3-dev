#pragma once

// Minimal FTP server for PS3 VSH. Anonymous, passive-mode only, binary
// transfers, RFC 3659 directory listings (MLSD / MLST / MDTM) — no
// LIST or NLST. One listener thread on port 21; each accepted client
// runs on its own PPU thread (up to FTP_MAX_SESSIONS concurrent).
//
// A "session" is one logged-in FTP client: one persistent control
// socket on :21 plus one data socket per transfer.
//
// Design choices vs webMAN / IRISMAN:
//   - Only MLSD/MLST/MDTM for listings. Every modern client (WinSCP,
//     FileZilla, lftp, curl, rclone) prefers MLSD when FEAT advertises
//     it. Dropping LIST/NLST removes ~125 lines of ls-l formatting and
//     the whole timezone-offset dance — MLSD is UTC by spec, the
//     client localizes.
//   - PASV picks an OS-assigned ephemeral port (bind to 0, read back
//     with getsockname). No port-range hunt, no RTC-seeded retry loop.
//   - No TYPE A translation. We accept the command but all transfers
//     are byte-exact. Modern clients default to binary anyway.
//   - shutdown() + socketclose() to unstick any blocking recv/accept.
//   - sys_net_errno (not POSIX errno) is the error variable.
//   - No SO_REUSEADDR. Control socket bind conflicts just fail cleanly.
//
// Protocol behaviour cross-checked against IRISMAN and webMAN-MOD;
// no code was copied from either — this is a fresh implementation.

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
// No <time.h>: libc's time()/gmtime() don't resolve reliably from a PRX
// and caused silent load failures. The Cell RTC API covers the same job
// — cellRtcSetTime_t() lives in librtc_stub and is PRX-safe.
// Requires -lrtc_stub in AdditionalDependencies.
#include <cell/rtc.h>

#include <sys/ppu_thread.h>
#include <sys/timer.h>
#include <sys/time.h>  // struct timeval for SO_RCVTIMEO

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netex/net.h>
#include <netex/errno.h>

#include "thread.h"
#include <netex/sockinfo.h>

#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#include "dbg.h"
#include "string-utilities.h"

// --- tuneables ---
enum {
    FTP_PORT           = 21,
    FTP_MAX_SESSIONS   = 2,           // WinSCP opens a 2nd control channel
                                      // for editor flows — 1 is too tight.
    FTP_BLOCK          = 128 * 1024,  // RETR/STOR chunk size. Also sized
                                      // to match SO_SNDBUF/SO_RCVBUF — a
                                      // 1 MB socket buffer regressed us.
    FTP_CMDBUF         = 1024,        // control line buffer
    FTP_PATHBUF        = 1024,
    FTP_DIRBUF         = 8 * 1024,    // MLSD staging
    FTP_CTRL_TIMEOUT_S = 600,         // idle control socket timeout
};

// --- per-session state ---
typedef struct {
    int      ctrl;                  // control socket (connected)
    int      pasv;                  // PASV listen socket, -1 if idle
    int      data;                  // accepted PASV data socket
    uint32_t localIp;               // our IP seen by this client, for 227
    char     cwd[FTP_PATHBUF];      // virtual current directory
    char     rnfr[FTP_PATHBUF];     // RNFR → RNTO carry
    char     cmdBuf[FTP_CMDBUF];    // accumulator for recv
    int      cmdLen;
    char     io[FTP_BLOCK];         // transfer buffer
    char     dirBuf[FTP_DIRBUF];    // MLSD staging
    volatile int alive;             // set to 0 on QUIT or error
} FtpSession;

// --- session pool. Static, one slot per allowed concurrent client. ---
static FtpSession sessionPool[FTP_MAX_SESSIONS];
static volatile int sessionPoolUsed[FTP_MAX_SESSIONS];

static FtpSession *acquireSession(void)
{
    for (int i = 0; i < FTP_MAX_SESSIONS; i++) {
        if (!sessionPoolUsed[i]) {
            sessionPoolUsed[i] = 1;
            return &sessionPool[i];
        }
    }
    return NULL;
}

static void releaseSession(FtpSession *session)
{
    int i = (int)(session - sessionPool);
    if (i >= 0 && i < FTP_MAX_SESSIONS) sessionPoolUsed[i] = 0;
}

// --- socket helpers ---

static void ftpCloseSock(int *fd)
{
    if (*fd >= 0) {
        shutdown(*fd, SHUT_RDWR);
        socketclose(*fd);
        *fd = -1;
    }
}

static int ftpListen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { socketclose(fd); return -1; }
    if (listen(fd, FTP_MAX_SESSIONS) < 0)              { socketclose(fd); return -1; }
    return fd;
}

// Send all. Returns 0 on success, -1 on error.
static int ftpSendAll(int fd, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int got = send(fd, buf + off, len - off, 0);
        if (got <= 0) return -1;
        off += got;
    }
    return 0;
}

// Write a CRLF-terminated reply: "<code> <msg>\r\n".
static void ftpReply(int fd, int code, const char *msg)
{
    char buf[512];
    int  n = 0;
    buf[n++] = (char)('0' + (code / 100) % 10);
    buf[n++] = (char)('0' + (code /  10) % 10);
    buf[n++] = (char)('0' + (code      ) % 10);
    buf[n++] = ' ';
    for (int i = 0; msg[i] && n < (int)sizeof(buf) - 2; i++) buf[n++] = msg[i];
    buf[n++] = '\r';
    buf[n++] = '\n';
    ftpSendAll(fd, buf, n);
}

// Emit a quoted-path reply (RFC 959 §5.1.3, PWD/MKD style).
static void replyQuotedPath(FtpSession *session, int code, const char *path)
{
    char buf[FTP_PATHBUF + 16];
    int n = 0;
    buf[n++] = (char)('0' + (code / 100) % 10);
    buf[n++] = (char)('0' + (code /  10) % 10);
    buf[n++] = (char)('0' +  code         % 10);
    buf[n++] = ' ';
    buf[n++] = '"';
    for (int i = 0; path[i] && n < (int)sizeof(buf) - 6; i++) {
        if (path[i] == '"') buf[n++] = '"';  // RFC quote-doubling
        buf[n++] = path[i];
    }
    buf[n++] = '"';
    buf[n++] = '\r';
    buf[n++] = '\n';
    ftpSendAll(session->ctrl, buf, n);
}

// --- path handling ---
// All paths are absolute virtual paths rooted at "/".  "/" maps to the
// PS3's real root (dev_hdd0, dev_flash, dev_usb etc. appear as top-level
// entries — cellFsReaddir on "/" returns these).  resolvePath joins an
// input against cwd and normalizes "." and ".." components.  No escape
// protection — we're the console owner, the whole FS is fair game.

static void normalizePath(char *path)
{
    // In-place: collapse //, resolve . and .., ensure leading /, strip trailing /.
    int n = strLen(path);
    char tmp[FTP_PATHBUF];
    int ti = 0;
    int i  = 0;

    if (path[0] != '/') { tmp[ti++] = '/'; }

    while (i < n) {
        while (i < n && path[i] == '/') i++;
        if (i >= n) break;

        int cStart = i;
        while (i < n && path[i] != '/') i++;
        int cLen = i - cStart;

        if (cLen == 1 && path[cStart] == '.') {
            // skip
        } else if (cLen == 2 && path[cStart] == '.' && path[cStart + 1] == '.') {
            if (ti > 1) {
                ti--;
                while (ti > 0 && tmp[ti - 1] != '/') ti--;
                if (ti > 1) ti--;
            }
        } else {
            if (ti == 0 || tmp[ti - 1] != '/') tmp[ti++] = '/';
            for (int k = 0; k < cLen && ti < FTP_PATHBUF - 1; k++)
                tmp[ti++] = path[cStart + k];
        }
    }
    if (ti == 0) tmp[ti++] = '/';
    tmp[ti] = 0;

    for (int k = 0; k <= ti; k++) path[k] = tmp[k];
}

static void resolvePath(FtpSession *session, const char *in, char *out)
{
    int o = 0;
    if (in[0] == '/') {
        while (in[o] && o < FTP_PATHBUF - 1) { out[o] = in[o]; o++; }
        out[o] = 0;
    } else {
        int i = 0;
        while (session->cwd[i] && o < FTP_PATHBUF - 1) { out[o++] = session->cwd[i++]; }
        if (o > 0 && out[o - 1] != '/' && o < FTP_PATHBUF - 1) out[o++] = '/';
        for (i = 0; in[i] && o < FTP_PATHBUF - 1; i++) out[o++] = in[i];
        out[o] = 0;
    }
    normalizePath(out);
}

// Join dir + name with a single separator.
static void joinPath(char *out, int cap, const char *dir, const char *name)
{
    int o = 0;
    while (dir[o] && o < cap - 1) { out[o] = dir[o]; o++; }
    if (o > 0 && out[o - 1] != '/' && o < cap - 1) out[o++] = '/';
    for (int k = 0; name[k] && o < cap - 1; k++) out[o++] = name[k];
    out[o] = 0;
}

// Write the parent of `in` (absolute path) into `out`. "/" stays "/".
static void derivePathParent(const char *in, char *out)
{
    int n = strLen(in);
    while (n > 1 && in[n - 1] == '/') n--;  // strip trailing /
    while (n > 0 && in[n - 1] != '/') n--;  // back to last /   
    if (n > 1) n--;  // drop that / unless root
    if (n == 0) { out[0] = '/'; out[1] = 0; return; }
    for (int i = 0; i < n; i++) out[i] = in[i];
    out[n] = 0;
}

// --- data connection (PASV) ---

// Open PASV listener on an OS-assigned port. Returns fd or -1.
static int openPasv(uint16_t *outPort)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = htons(0);
    a.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { socketclose(fd); return -1; }
    if (listen(fd, 1) < 0)                             { socketclose(fd); return -1; }

    socklen_t alen = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &alen) < 0) {
        socketclose(fd);
        return -1;
    }
    *outPort = ntohs(a.sin_port);
    return fd;
}

// Wait for the client's data connection. Returns accepted fd or -1.
static int acceptPasv(FtpSession *session)
{
    if (session->pasv < 0) return -1;
    struct sockaddr_in ra;
    socklen_t ralen = sizeof ra;
    int d = accept(session->pasv, (struct sockaddr *)&ra, &ralen);
    ftpCloseSock(&session->pasv);
    return d;
}

// --- MLSD / MLST / MDTM (RFC 3659) ---
// Line format:
//   type=X;size=N;modify=YYYYMMDDHHMMSS;UNIX.mode=0NNN;UNIX.uid=nobody;UNIX.gid=nobody; name\r\n
// Directories use sizd= instead of size=. Timestamps are UTC per spec.

// Format UTC mtime as YYYYMMDDHHMMSS (14 chars, no NUL).
static void formatMlsdTime(uint64_t mtime, char *out)
{
    CellRtcDateTime d;
    if (cellRtcSetTime_t(&d, (time_t)mtime) != 0) {
        d.year = 1970; d.month = 1; d.day = 1;
        d.hour = 0;    d.minute = 0; d.second = 0;
    }
    int y  = (int)d.year;   if (y  < 1970 || y  > 9999) y  = 1970;
    int mo = (int)d.month;  if (mo < 1    || mo > 12)   mo = 1;
    int da = (int)d.day;    if (da < 1    || da > 31)   da = 1;
    int hh = (int)d.hour;   if (hh < 0    || hh > 23)   hh = 0;
    int mi = (int)d.minute; if (mi < 0    || mi > 59)   mi = 0;
    int se = (int)d.second; if (se < 0    || se > 59)   se = 0;

    int o = 0;
    out[o++] = (char)('0' + (y  / 1000) % 10);
    out[o++] = (char)('0' + (y  /  100) % 10);
    out[o++] = (char)('0' + (y  /   10) % 10);
    out[o++] = (char)('0' +  y          % 10);
    out[o++] = (char)('0' + (mo / 10) % 10); out[o++] = (char)('0' + mo % 10);
    out[o++] = (char)('0' + (da / 10) % 10); out[o++] = (char)('0' + da % 10);
    out[o++] = (char)('0' + (hh / 10) % 10); out[o++] = (char)('0' + hh % 10);
    out[o++] = (char)('0' + (mi / 10) % 10); out[o++] = (char)('0' + mi % 10);
    out[o++] = (char)('0' + (se / 10) % 10); out[o++] = (char)('0' + se % 10);  // o == 14
}

// Emit one MLSD entry into buf. `type` is "file", "dir", "cdir", or "pdir".
// Caller is responsible for flushing ahead of this to ensure room; the
// internal bounds checks are a safety net against runaway names.
static void appendMlsdLine(char *buf, int cap, int *off,
                           const char *type, uint64_t size, uint32_t mode,
                           uint64_t mtime, const char *name)
{
    int o = *off;

    appendStr(buf, cap, &o, "type=");
    appendStr(buf, cap, &o, type);
    if (o < cap) buf[o++] = ';';

    // Directories use sizd= (size-of-dir); files use size=.
    int isDirType = (type[0] == 'd' || type[0] == 'c' || type[0] == 'p');
    appendStr(buf, cap, &o, isDirType ? "sizd=" : "size=");
    o = appendUint64(buf, cap, o, size);
    if (o < cap) buf[o++] = ';';

    appendStr(buf, cap, &o, "modify=");
    if (o + 14 <= cap) { formatMlsdTime(mtime, buf + o); o += 14; }
    if (o < cap) buf[o++] = ';';

    appendStr(buf, cap, &o, "UNIX.mode=0");
    uint32_t m = mode & 0777;
    if (o + 3 <= cap) {
        buf[o++] = (char)('0' + ((m >> 6) & 7));
        buf[o++] = (char)('0' + ((m >> 3) & 7));
        buf[o++] = (char)('0' +  (m       & 7));
    }
    if (o < cap) buf[o++] = ';';

    appendStr(buf, cap, &o, "UNIX.uid=nobody;UNIX.gid=nobody; ");

    for (int i = 0; name[i] && o < cap - 2; i++) buf[o++] = name[i];
    if (o + 2 <= cap) { buf[o++] = '\r'; buf[o++] = '\n'; }
    *off = o;
}

// Is the stat a directory?
static int statIsDir(const CellFsStat *st)
{
    return (st->st_mode & CELL_FS_S_IFDIR) ? 1 : 0;
}

// Emit one MLSD entry from a stat. Flushes first if near-full. Returns -1
// only on send() failure — a silently-dropped overlong name is not fatal.
static int emitMlsdEntry(int dataFd, char *buf, int cap, int *off,
                         const char *type, const CellFsStat *st, const char *name)
{
    int needed = strLen(name) + 128;  // facts + CRLF + cushion
    if (*off > 0 && *off + needed > cap) {
        if (ftpSendAll(dataFd, buf, *off) < 0) return -1;
        *off = 0;
    }
    appendMlsdLine(buf, cap, off, type, st->st_size,
                   (uint32_t)(st->st_mode & 0777),
                   (uint64_t)st->st_mtime, name);
    return 0;
}

// Stream a directory as MLSD. Emits cdir (.) and pdir (..) entries per
// RFC 3659 §7.5.1 so clients can stat the directory itself and its parent.
static int streamDirMlsd(int dataFd, const char *path, char *buf, int cap)
{
    int off = 0;

    // cdir: stat the directory we're listing. Skip silently if "/" (PS3
    // doesn't stat the virtual root).
    CellFsStat st;
    if (cellFsStat(path, &st) == CELL_FS_SUCCEEDED) {
        if (emitMlsdEntry(dataFd, buf, cap, &off, "cdir", &st, ".") < 0) return -1;
    }

    // pdir: stat the parent (unless we're at root).
    if (!(path[0] == '/' && path[1] == 0)) {
        char parent[FTP_PATHBUF];
        derivePathParent(path, parent);
        CellFsStat pst;
        if (cellFsStat(parent, &pst) == CELL_FS_SUCCEEDED) {
            if (emitMlsdEntry(dataFd, buf, cap, &off, "pdir", &pst, "..") < 0) return -1;
        }
    }

    int fd;
    if (cellFsOpendir(path, &fd) != CELL_FS_SUCCEEDED) {
        if (off > 0) ftpSendAll(dataFd, buf, off);
        return -1;
    }

    CellFsDirent ent;
    uint64_t entSz;
    while (cellFsReaddir(fd, &ent, &entSz) == CELL_FS_SUCCEEDED && entSz > 0) {
        // Skip "." and ".." — emitted as cdir/pdir above.
        if (ent.d_name[0] == '.' &&
            (ent.d_name[1] == 0 ||
             (ent.d_name[1] == '.' && ent.d_name[2] == 0)))
            continue;

        char full[FTP_PATHBUF];
        joinPath(full, FTP_PATHBUF, path, ent.d_name);

        CellFsStat est;
        if (cellFsStat(full, &est) != CELL_FS_SUCCEEDED) continue;

        const char *type = statIsDir(&est) ? "dir" : "file";
        if (emitMlsdEntry(dataFd, buf, cap, &off, type, &est, ent.d_name) < 0) {
            cellFsClosedir(fd);
            return -1;
        }
    }
    cellFsClosedir(fd);
    if (off > 0) return ftpSendAll(dataFd, buf, off);
    return 0;
}

// --- command handlers ---

static void handleUser(FtpSession *session, const char *arg)
{
    (void)arg;
    ftpReply(session->ctrl, 331, "User name okay, need password.");
}

static void handlePass(FtpSession *session, const char *arg)
{
    (void)arg;
    ftpReply(session->ctrl, 230, "Anonymous login accepted.");
}

static void handleSyst(FtpSession *session, const char *arg)
{
    (void)arg;
    ftpReply(session->ctrl, 215, "UNIX Type: L8");
}

static void handleFeat(FtpSession *session, const char *arg)
{
    (void)arg;
    // MLST facts line: '*' marks labels enabled by default (RFC 3659 §7.8).
    // No LIST/NLST — callers must use MLSD for listings.
    const char *feat =
        "211-Features:\r\n"
        " SIZE\r\n"
        " MDTM\r\n"
        " MLSD\r\n"
        " MLST type*;size*;sizd*;modify*;UNIX.mode*;UNIX.uid*;UNIX.gid*;\r\n"
        " UTF8\r\n"
        "211 End.\r\n";
    ftpSendAll(session->ctrl, feat, strLen(feat));
}

static void handleNoop(FtpSession *session, const char *arg)
{
    (void)arg;
    ftpReply(session->ctrl, 200, "OK");
}

static void handleQuit(FtpSession *session, const char *arg)
{
    (void)arg;
    ftpReply(session->ctrl, 221, "Goodbye.");
    session->alive = 0;
}

static void handleType(FtpSession *session, const char *arg)
{
    // Accept any TYPE; treat all as binary.
    (void)arg;
    ftpReply(session->ctrl, 200, "Type set to binary.");
}

static void handlePwd(FtpSession *session, const char *arg)
{
    (void)arg;
    replyQuotedPath(session, 257, session->cwd);
}

static void handleCwd(FtpSession *session, const char *arg)
{
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);
    CellFsStat st;
    if (cellFsStat(tgt, &st) != CELL_FS_SUCCEEDED && !strEq(tgt, "/")) {
        ftpReply(session->ctrl, 550, "Directory not found.");
        return;
    }
    strCopy(session->cwd, FTP_PATHBUF, tgt);
    ftpReply(session->ctrl, 250, "Directory changed.");
}

static void handleCdup(FtpSession *session, const char *arg)
{
    (void)arg;
    handleCwd(session, "..");
}

static void handlePasv(FtpSession *session, const char *arg)
{
    (void)arg;
    ftpCloseSock(&session->pasv);
    ftpCloseSock(&session->data);

    uint16_t port;
    int fd = openPasv(&port);
    if (fd < 0) { ftpReply(session->ctrl, 425, "Cannot open passive port."); return; }
    session->pasv = fd;

    // localIp is in network byte order — read bytes directly so the octet
    // order is correct on both big- and little-endian. Arithmetic shifts
    // on the uint32_t would reverse the octets on big-endian PPU.
    const unsigned char *ipB = (const unsigned char *)&session->localIp;
    unsigned char p1 = (unsigned char)(port >> 8);
    unsigned char p2 = (unsigned char)(port & 0xff);

    char line[96];
    int n = 0;
    appendStr(line, (int)sizeof line, &n, "227 Entering Passive Mode (");
    unsigned char parts[6] = { ipB[0], ipB[1], ipB[2], ipB[3], p1, p2 };
    for (int i = 0; i < 6; i++) {
        unsigned v = parts[i];
        if (v >= 100) {
            line[n++] = (char)('0' + v / 100); v %= 100;
            line[n++] = (char)('0' + v / 10);
            line[n++] = (char)('0' + v % 10);
        } else if (v >= 10) {
            line[n++] = (char)('0' + v / 10);
            line[n++] = (char)('0' + v % 10);
        } else {
            line[n++] = (char)('0' + v);
        }
        line[n++] = (i == 5) ? ')' : ',';
    }
    line[n++] = '\r';
    line[n++] = '\n';
    ftpSendAll(session->ctrl, line, n);
}

static void handleMlsd(FtpSession *session, const char *arg)
{
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);  // empty arg → cwd
    int d = acceptPasv(session);
    if (d < 0) { ftpReply(session->ctrl, 425, "No data connection."); return; }

    ftpReply(session->ctrl, 150, "Opening data connection.");
    int rc = streamDirMlsd(d, tgt, session->dirBuf, FTP_DIRBUF);
    ftpCloseSock(&d);
    ftpReply(session->ctrl, rc == 0 ? 226 : 550,
             rc == 0 ? "Transfer complete." : "Directory read failed.");
}

// MLST: single-entry listing on the control channel (no data conn).
// RFC 3659 §4.3.2 format:
//     250-Listing /path\r\n
//      type=X;size=N;modify=...;UNIX.mode=...;UNIX.uid=nobody;UNIX.gid=nobody; name\r\n
//     250 End\r\n
// The middle line MUST begin with a single SP.
static void handleMlst(FtpSession *session, const char *arg)
{
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);  // empty arg → cwd
    CellFsStat st;
    if (cellFsStat(tgt, &st) != CELL_FS_SUCCEEDED) {
        ftpReply(session->ctrl, 550, "File not found.");
        return;
    }

    // Path appears twice (prefix header + entry name), so size generously.
    char buf[2 * FTP_PATHBUF + 256];
    int o = 0;
    appendStr(buf, (int)sizeof buf, &o, "250-Listing ");
    appendStr(buf, (int)sizeof buf, &o, tgt);
    buf[o++] = '\r'; buf[o++] = '\n';

    buf[o++] = ' ';  // required leading SP
    const char *type = statIsDir(&st) ? "dir" : "file";
    appendMlsdLine(buf, (int)sizeof buf, &o, type, st.st_size,
                   (uint32_t)(st.st_mode & 0777),
                   (uint64_t)st.st_mtime, tgt);

    appendStr(buf, (int)sizeof buf, &o, "250 End\r\n");
    ftpSendAll(session->ctrl, buf, o);
}

// MDTM: "213 YYYYMMDDHHMMSS\r\n" in UTC.
static void handleMdtm(FtpSession *session, const char *arg)
{
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);
    CellFsStat st;
    if (cellFsStat(tgt, &st) != CELL_FS_SUCCEEDED) {
        ftpReply(session->ctrl, 550, "File not found.");
        return;
    }
    char line[32];
    int n = 0;
    line[n++] = '2'; line[n++] = '1'; line[n++] = '3'; line[n++] = ' ';
    formatMlsdTime((uint64_t)st.st_mtime, line + n);
    n += 14;
    line[n++] = '\r';
    line[n++] = '\n';
    ftpSendAll(session->ctrl, line, n);
}

static void handleRetr(FtpSession *session, const char *arg)
{
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);

    int fd;
    if (cellFsOpen(tgt, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        ftpReply(session->ctrl, 550, "Open failed.");
        return;
    }
    int d = acceptPasv(session);
    if (d < 0) { cellFsClose(fd); ftpReply(session->ctrl, 425, "No data connection."); return; }

    // Size the TCP send buffer to the chunk size so send() doesn't stall
    // between cellFsRead calls — PS3's default SO_SNDBUF is small.
    // Tried 1 MB; measured slightly worse (18.3 vs 19.3 MB/session), reverted.
    int bufSz = FTP_BLOCK;
    setsockopt(d, SOL_SOCKET, SO_SNDBUF, &bufSz, sizeof bufSz);

    ftpReply(session->ctrl, 150, "Opening data connection.");
    int err = 0;
    for (;;) {
        uint64_t got = 0;
        if (cellFsRead(fd, session->io, FTP_BLOCK, &got) != CELL_FS_SUCCEEDED) { err = 1; break; }
        if (got == 0) break;
        if (ftpSendAll(d, session->io, (int)got) < 0) { err = 1; break; }
    }
    cellFsClose(fd);
    ftpCloseSock(&d);
    ftpReply(session->ctrl, err ? 550 : 226, err ? "Transfer aborted." : "Transfer complete.");
}

static void handleStorOrAppe(FtpSession *session, const char *arg, int append)
{
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);

    int flags = CELL_FS_O_WRONLY | CELL_FS_O_CREAT |
                (append ? CELL_FS_O_APPEND : CELL_FS_O_TRUNC);
    int fd;
    if (cellFsOpen(tgt, flags, &fd, NULL, 0) != CELL_FS_SUCCEEDED) {
        ftpReply(session->ctrl, 550, "Open failed.");
        return;
    }
    int d = acceptPasv(session);
    if (d < 0) { cellFsClose(fd); ftpReply(session->ctrl, 425, "No data connection."); return; }

    // Mirror RETR: size the TCP receive buffer so recv() doesn't backpressure
    // the client between cellFsWrite calls.
    int bufSz = FTP_BLOCK;
    setsockopt(d, SOL_SOCKET, SO_RCVBUF, &bufSz, sizeof bufSz);

    ftpReply(session->ctrl, 150, "Opening data connection.");
    int err = 0;
    for (;;) {
        int n = recv(d, session->io, FTP_BLOCK, 0);
        if (n < 0) { err = 1; break; }
        if (n == 0) break;
        uint64_t wrote = 0;
        if (cellFsWrite(fd, session->io, (uint64_t)n, &wrote) != CELL_FS_SUCCEEDED
            || wrote != (uint64_t)n) { err = 1; break; }
    }
    cellFsClose(fd);
    ftpCloseSock(&d);
    ftpReply(session->ctrl, err ? 550 : 226, err ? "Transfer aborted." : "Transfer complete.");
}

static void handleStor(FtpSession *session, const char *arg) { handleStorOrAppe(session, arg, 0); }

static void handleAppe(FtpSession *session, const char *arg) { handleStorOrAppe(session, arg, 1); }

static void handleSize(FtpSession *session, const char *arg)
{
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);
    CellFsStat st;
    if (cellFsStat(tgt, &st) != CELL_FS_SUCCEEDED) {
        ftpReply(session->ctrl, 550, "File not found.");
        return;
    }
    char line[64];
    int n = 0;
    line[n++] = '2'; line[n++] = '1'; line[n++] = '3'; line[n++] = ' ';
    n = appendUint64(line, (int)sizeof line, n, (uint64_t)st.st_size);
    line[n++] = '\r';
    line[n++] = '\n';
    ftpSendAll(session->ctrl, line, n);
}

static void handleDele(FtpSession *session, const char *arg)
{
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);
    if (cellFsUnlink(tgt) == CELL_FS_SUCCEEDED)
        ftpReply(session->ctrl, 250, "File deleted.");
    else
        ftpReply(session->ctrl, 550, "Delete failed.");
}

static void handleMkd(FtpSession *session, const char *arg)
{
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);
    if (cellFsMkdir(tgt, CELL_FS_S_IFDIR | 0777) != CELL_FS_SUCCEEDED) {
        ftpReply(session->ctrl, 550, "Mkdir failed.");
        return;
    }
    replyQuotedPath(session, 257, tgt);
}

static void handleRmd(FtpSession *session, const char *arg)
{
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);
    if (cellFsRmdir(tgt) == CELL_FS_SUCCEEDED)
        ftpReply(session->ctrl, 250, "Directory removed.");
    else
        ftpReply(session->ctrl, 550, "Rmdir failed.");
}

static void handleRnfr(FtpSession *session, const char *arg)
{
    resolvePath(session, arg, session->rnfr);
    CellFsStat st;
    if (cellFsStat(session->rnfr, &st) != CELL_FS_SUCCEEDED) {
        session->rnfr[0] = 0;
        ftpReply(session->ctrl, 550, "File not found.");
        return;
    }
    ftpReply(session->ctrl, 350, "Ready for RNTO.");
}

static void handleRnto(FtpSession *session, const char *arg)
{
    if (session->rnfr[0] == 0) { ftpReply(session->ctrl, 503, "Send RNFR first."); return; }
    char tgt[FTP_PATHBUF];
    resolvePath(session, arg, tgt);
    if (cellFsRename(session->rnfr, tgt) == CELL_FS_SUCCEEDED)
        ftpReply(session->ctrl, 250, "Rename complete.");
    else
        ftpReply(session->ctrl, 550, "Rename failed.");
    session->rnfr[0] = 0;
}

typedef struct {
    const char *name;
    void (*handler)(FtpSession *, const char *);
} FtpCmd;

static const FtpCmd ftpCmds[] = {
    { "USER", handleUser }, { "PASS", handlePass },
    { "SYST", handleSyst }, { "FEAT", handleFeat },
    { "NOOP", handleNoop }, { "QUIT", handleQuit },
    { "TYPE", handleType },
    { "PWD",  handlePwd  }, { "XPWD", handlePwd  },
    { "CWD",  handleCwd  }, { "CDUP", handleCdup },
    { "PASV", handlePasv },
    { "MLSD", handleMlsd }, { "MLST", handleMlst }, { "MDTM", handleMdtm },
    { "RETR", handleRetr },
    { "STOR", handleStor }, { "APPE", handleAppe },
    { "SIZE", handleSize },
    { "DELE", handleDele },
    { "MKD",  handleMkd  }, { "XMKD", handleMkd  },
    { "RMD",  handleRmd  }, { "XRMD", handleRmd  },
    { "RNFR", handleRnfr }, { "RNTO", handleRnto },
};

// --- session main loop ---
// Reads CRLF-terminated commands from cmdBuf, dispatches via ftpCmds.

static void ftpDispatch(FtpSession *session, char *line)
{
    char *sp = line;
    while (*sp && *sp != ' ') sp++;
    char verb[8];
    int vl = (int)(sp - line);
    if (vl > 7) vl = 7;
    for (int i = 0; i < vl; i++) {
        char c = line[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        verb[i] = c;
    }
    verb[vl] = 0;
    const char *arg = (*sp == ' ') ? sp + 1 : "";

    int n = (int)(sizeof(ftpCmds) / sizeof(ftpCmds[0]));
    for (int i = 0; i < n; i++) {
        if (strEq(verb, ftpCmds[i].name)) {
            ftpCmds[i].handler(session, arg);
            return;
        }
    }
    ftpReply(session->ctrl, 502, "Command not implemented.");
}

static void runFtpSession(FtpSession *session)
{
    // Learn our local IP from the accepted control socket (Sony ext).
    sys_net_sockinfo_t si;
    if (sys_net_get_sockinfo(session->ctrl, &si, 1) >= 0) {
        session->localIp = (uint32_t)si.local_adr.s_addr;  // network byte order
    }

    // Idle timeout on control socket.
    struct timeval tv;
    tv.tv_sec  = FTP_CTRL_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(session->ctrl, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    ftpReply(session->ctrl, 220, "simple-ftp ready — anonymous access, binary mode.");

    session->alive  = 1;
    session->cmdLen = 0;
    strCopy(session->cwd, FTP_PATHBUF, "/");
    session->rnfr[0] = 0;
    session->pasv = -1;
    session->data = -1;

    while (session->alive) {
        int space = FTP_CMDBUF - session->cmdLen - 1;
        if (space <= 0) { session->cmdLen = 0; space = FTP_CMDBUF - 1; }  // overflow: drop
        int n = recv(session->ctrl, session->cmdBuf + session->cmdLen, space, 0);
        if (n <= 0) break;
        session->cmdLen += n;

        // Extract complete CRLF (or LF) lines.
        int scan = 0;
        while (scan < session->cmdLen) {
            int eol = scan;
            while (eol < session->cmdLen && session->cmdBuf[eol] != '\n') eol++;
            if (eol >= session->cmdLen) break;
            int lineEnd = eol;
            if (lineEnd > scan && session->cmdBuf[lineEnd - 1] == '\r') lineEnd--;
            session->cmdBuf[lineEnd] = 0;
            ftpDispatch(session, session->cmdBuf + scan);
            scan = eol + 1;
            if (!session->alive) break;
        }
        if (scan > 0) {
            int rem = session->cmdLen - scan;
            for (int i = 0; i < rem; i++) session->cmdBuf[i] = session->cmdBuf[scan + i];
            session->cmdLen = rem;
        }
    }

    ftpCloseSock(&session->data);
    ftpCloseSock(&session->pasv);
    ftpCloseSock(&session->ctrl);
}

// Thread entry. arg = pointer to FtpSession (already acquired & ctrl-set).
static void ftpSessionThread(uint64_t arg)
{
    FtpSession *session = (FtpSession *)(uintptr_t)arg;
    runFtpSession(session);
    releaseSession(session);
    exitThread();
}

// --- listener (spawned from prx.c) ---

static void ftpListenerThread(uint64_t arg)
{
    (void)arg;

    int listenFd = -1;
    int retries  = 0;
    while (listenFd < 0 && retries < 30) {
        listenFd = ftpListen(FTP_PORT);
        if (listenFd < 0) {
            logWarn("[ftp] listen failed, retrying\n");
            sys_timer_sleep(2);
            retries++;
        }
    }
    if (listenFd < 0) {
        logError("[ftp] giving up — port 21 unavailable\n");
        exitThread();
        return;
    }
    logInfo("[ftp] listening on :21\n");

    for (;;) {
        struct sockaddr_in ra;
        socklen_t ralen = sizeof ra;
        int c = accept(listenFd, (struct sockaddr *)&ra, &ralen);
        if (c < 0) { sys_timer_usleep(100000); continue; }

        FtpSession *session = acquireSession();
        if (!session) {
            ftpReply(c, 421, "Too many connections.");
            shutdown(c, SHUT_RDWR);
            socketclose(c);
            continue;
        }
        session->ctrl = c;
        session->pasv = -1;
        session->data = -1;

        sys_ppu_thread_t tid;
        int r = sys_ppu_thread_create(&tid, ftpSessionThread,
                                      (uint64_t)(uintptr_t)session,
                                      0x500, 0x4000,
                                      SYS_PPU_THREAD_CREATE_JOINABLE, "ftpc");
        if (r != 0) {
            ftpReply(c, 421, "Server busy.");
            shutdown(c, SHUT_RDWR);
            socketclose(c);
            releaseSession(session);
        }
    }
}
