#pragma once

// file-ops commands: get-file, save-file, delete-file, list-dir.
// thin wrappers around the helpers in fileio.h that frame the result
// for the host socket and parse the argument tokens.

#include "cmd-common.h"
#include "fileio.h"

// get-file <path> [offset length]
//   length=0 means "until end-of-file".
//   reply: OK <window>\n<window bytes> on success, ERR ... on failure.
static void cmdGetFile(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    const char *rest = parsePath(args, path, sizeof path);
    if (!rest) {
        sendReply(cli, SDB_ERR, "usage: get-file <path> [offset length]");
        return;
    }

    uint64_t offset = 0, length = 0;
    if (*rest) {
        rest = parseUInt64(rest, &offset);
        if (!rest) { sendReply(cli, SDB_ERR, "bad offset"); return; }
        rest = parseUInt64(rest, &length);
        if (!rest) { sendReply(cli, SDB_ERR, "bad length"); return; }
    }

    if (!fileExists(path)) {
        sendReply(cli, SDB_ERR, "no such file");
        return;
    }
    int64_t window = fileWindowSize(path, offset, length);
    if (window < 0) {
        sendReply(cli, SDB_ERR, "offset past end of file");
        return;
    }

    if (sendFrameHeader(cli, SDB_OK, (uint32_t)window) < 0) return;

    if (window > 0 && sendFileWindow(cli, path, offset, (uint64_t)window) < 0) {
        // header already on the wire; client gets short read and reports it.
        logError("[sdb] get-file send truncated\n");
    }
}

// save-file <path> <size>\n<size raw bytes>
//   reply: OK saved <path> (<size> bytes)  or  ERR ...
//   does NOT auto-create parent directories - caller picks an existing path.
static void cmdSaveFile(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    const char *rest = parsePath(args, path, sizeof path);
    if (!rest) {
        sendReply(cli, SDB_ERR, "usage: save-file <path> <size>");
        return;
    }
    uint64_t size = 0;
    rest = parseUInt64(rest, &size);
    if (!rest) {
        sendReply(cli, SDB_ERR, "bad size");
        return;
    }
    if (recvFile(cli, path, (uint32_t)size) < 0) {
        sendReply(cli, SDB_ERR, "save failed");
        return;
    }
    char reply[FILE_PATH_MAX + 64];
    snprintf(reply, sizeof reply, "saved %s (%llu bytes)", path, (unsigned long long)size);
    sendReply(cli, SDB_OK, reply);
}

// delete-file <path>
//   reply: OK deleted <path>  or  ERR ...
//   missing files are treated as success (idempotent).
static void cmdDeleteFile(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    if (!parsePath(args, path, sizeof path)) {
        sendReply(cli, SDB_ERR, "usage: delete-file <path>");
        return;
    }
    if (deleteFile(path) < 0) {
        sendReply(cli, SDB_ERR, "delete failed");
        return;
    }
    char reply[FILE_PATH_MAX + 64];
    snprintf(reply, sizeof reply, "deleted %s", path);
    sendReply(cli, SDB_OK, reply);
}

// list-dir <path>
//   reply: OK <n>\n<n bytes> with one "<kind>\t<size>\t<mtime>\t<name>\n"
//   line per entry. used to diff /dev_hdd0/ before and after a sony-side
//   install so we can find what xmb registration writes that we don't.
static void cmdListDir(int cli, const char *args)
{
    char path[FILE_PATH_MAX];
    if (!parsePath(args, path, sizeof path)) {
        sendReply(cli, SDB_ERR, "usage: list-dir <path>");
        return;
    }
    int n = listDir(path, replyBuf, REPLY_BUF_BYTES);
    if (n < 0) {
        sendReply(cli, SDB_ERR, "list failed");
        return;
    }
    if (sendFrameHeader(cli, SDB_OK, (uint32_t)n) < 0) return;
    if (n > 0) sendBytes(cli, replyBuf, n);
}
