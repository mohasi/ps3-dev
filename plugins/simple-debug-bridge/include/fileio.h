#pragma once

// generic ps3 file io primitives over a tcp socket. no protocol awareness —
// callers (server.h) own the wire framing. used by both raw file commands
// (pull-file / push-file) and higher-level wrappers (plugin.h, pkg.h).
// note: non-streaming primitives (readFile/writeFile/deleteFile/...) live in
// the shared prx lib's file.h — this header only adds the socket-coupled ones.

#include <stdint.h>
#include <sys/socket.h>
#include <cell/fs/cell_fs_file_api.h>
#include <cell/fs/cell_fs_errno.h>

#define FILE_PATH_MAX  512
#define FILE_CHUNK     4096

// stream `size` bytes from socket `cli` into `path` (truncating).
// caller is responsible for ensuring the parent directory exists.
// returns 0 on success, -1 on any io failure.
static int recvFile(int cli, const char *path, uint32_t size)
{
    int fd;
    if (cellFsOpen(path, CELL_FS_O_WRONLY | CELL_FS_O_CREAT | CELL_FS_O_TRUNC,
                   &fd, NULL, 0) != CELL_FS_SUCCEEDED) return -1;

    static char chunk[FILE_CHUNK];
    uint32_t remaining = size;
    while (remaining > 0) {
        int want = (int)(remaining < sizeof chunk ? remaining : sizeof chunk);
        int got = recv(cli, chunk, want, 0);
        if (got <= 0) { cellFsClose(fd); return -1; }
        uint64_t written = 0;
        if (cellFsWrite(fd, chunk, (uint64_t)got, &written) != CELL_FS_SUCCEEDED ||
            written != (uint64_t)got) {
            cellFsClose(fd);
            return -1;
        }
        remaining -= (uint32_t)got;
    }
    cellFsClose(fd);
    return 0;
}

// resolve [offset, offset+length) against `path`. length=0 means "to end".
// returns the actual byte count to send, or -1 if the file is missing /
// the offset is past EOF. lets callers size a "<n>"-style header before
// committing to streaming bytes.
static int64_t fileWindowSize(const char *path, uint64_t offset, uint64_t length)
{
    CellFsStat st;
    if (cellFsStat(path, &st) != CELL_FS_SUCCEEDED) return -1;
    uint64_t total = st.st_size;
    if (offset > total) return -1;
    uint64_t avail = total - offset;
    if (length == 0 || length > avail) length = avail;
    return (int64_t)length;
}

// stream `length` bytes of `path` starting at `offset` to socket `cli`.
// returns 0 on success, -1 on io failure (caller should already have
// committed to the response header by this point).
static int sendFileWindow(int cli, const char *path, uint64_t offset, uint64_t length)
{
    int fd;
    if (cellFsOpen(path, CELL_FS_O_RDONLY, &fd, NULL, 0) != CELL_FS_SUCCEEDED) return -1;

    if (offset > 0) {
        uint64_t pos = 0;
        if (cellFsLseek(fd, (int64_t)offset, CELL_FS_SEEK_SET, &pos) != CELL_FS_SUCCEEDED) {
            cellFsClose(fd);
            return -1;
        }
    }

    static char chunk[FILE_CHUNK];
    uint64_t remaining = length;
    while (remaining > 0) {
        uint64_t want = remaining < sizeof chunk ? remaining : sizeof chunk;
        uint64_t got = 0;
        if (cellFsRead(fd, chunk, want, &got) != CELL_FS_SUCCEEDED || got == 0) {
            cellFsClose(fd);
            return -1;
        }
        const char *p = chunk;
        uint64_t left = got;
        while (left > 0) {
            int n = send(cli, p, (int)left, 0);
            if (n <= 0) { cellFsClose(fd); return -1; }
            p += n;
            left -= (uint64_t)n;
        }
        remaining -= got;
    }
    cellFsClose(fd);
    return 0;
}

// list one directory into `out`, one entry per line:
//   "<kind>\t<size>\t<mtime>\t<name>\n"
// where kind is 'f' (regular), 'd' (directory), or '?' (other / stat failed).
// returns total bytes written on success (0 == empty dir), or -1 on failure
// (open or buffer-too-small). entries are stat'd individually so size/mtime
// reflect the underlying inode, matching what an ftp ls would show.
static int listDir(const char *dir, char *out, int cap)
{
    int fd;
    if (cellFsOpendir(dir, &fd) != CELL_FS_SUCCEEDED) return -1;

    int written = 0;
    CellFsDirent ent;
    uint64_t read = 0;
    while (cellFsReaddir(fd, &ent, &read) == CELL_FS_SUCCEEDED && read > 0) {
        const char *name = ent.d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;

        char path[FILE_PATH_MAX];
        if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) continue;

        char kind = '?';
        uint64_t size = 0;
        int64_t  mtime = 0;
        CellFsStat st;
        if (cellFsStat(path, &st) == CELL_FS_SUCCEEDED) {
            uint32_t mode = st.st_mode & CELL_FS_S_IFMT;
            if      (mode == CELL_FS_S_IFREG) kind = 'f';
            else if (mode == CELL_FS_S_IFDIR) kind = 'd';
            size  = st.st_size;
            mtime = (int64_t)st.st_mtime;
        }

        int n = snprintf(out + written, cap - written,
                         "%c\t%llu\t%lld\t%s\n",
                         kind, (unsigned long long)size, (long long)mtime, name);
        if (n < 0 || n >= cap - written) { cellFsClosedir(fd); return -1; }
        written += n;
    }
    cellFsClosedir(fd);
    return written;
}
