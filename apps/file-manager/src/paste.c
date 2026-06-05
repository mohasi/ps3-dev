// paste - move/copy the clipboard's items into a destination (see paste.h).
#include "paste.h"
#include "clipboard.h"
#include "file.h"
#include "file-task.h"
#include "string-utilities.h"

#define COPY_BUF_SIZE (64 * 1024)

static char copyBuf[COPY_BUF_SIZE];
static char destDir[MAX_PATH_LEN];
static int  replaceOnConflict = 1;   // file-leaf collisions during a merge: 1 replace, 0 keep

void setPasteDest(const char *dir)  { strCopy(destDir, sizeof destDir, dir); }
void setPasteReplaceOnConflict(int replace)  { replaceOnConflict = replace ? 1 : 0; }

// true when src already lives directly in destDir, i.e. this paste targets the
// item's own directory. a copy there duplicates with a suffix; a move is a no-op.
static int pastingIntoOwnDir(const char *src)
{
    char parent[MAX_PATH_LEN];
    strCopy(parent, sizeof parent, src);
    toParentPath(parent);
    return strEq(parent, destDir);
}

// writes destDir/<name> into out, but if that already exists, finds the lowest
// n >= 1 such that "<stem> (n)<ext>" is free and uses that instead. for files
// the extension (text after the last interior dot) is preserved; directories
// keep their whole name as the stem. used for copies back into the source dir, so
// duplicates land beside the original as "report (1).txt", "report (2).txt", ...
static void uniqueDest(char *out, int cap, const char *dir, const char *name, int isDirEntry)
{
    joinPath(out, cap, dir, name);
    if (!fileExists(out)) return;

    // split name into stem + extension (last interior dot; never a leading dot).
    int dot = -1;
    if (!isDirEntry)
        for (int i = getStrLen(name) - 1; i > 0; i--)
            if (name[i] == '.') { dot = i; break; }

    char stem[MAX_PATH_LEN];
    strCopy(stem, sizeof stem, name);
    const char *ext = "";
    if (dot >= 0) { stem[dot] = '\0'; ext = name + dot; }  // ext keeps its '.'

    char cand[MAX_PATH_LEN];
    for (int n = 1; ; n++) {
        int o = 0;
        appendStr(cand, sizeof cand, &o, stem);
        appendStr(cand, sizeof cand, &o, " (");
        o += intToDec(n, cand + o);
        appendStr(cand, sizeof cand, &o, ")");
        appendStr(cand, sizeof cand, &o, ext);
        cand[o] = '\0';
        joinPath(out, cap, dir, cand);
        if (!fileExists(out)) return;
    }
}

// size of item i for progress: its carried size when exact, otherwise a fresh
// walk (the only case the pre-fetched listing couldn't size up front).
static uint64_t itemBytes(int i)
{
    return isClipboardSizeExact(i) ? getClipboardSize(i) : measureTree(getClipboardPath(i), isCancelRequested);
}

int countClipboardConflicts(int cap)
{
    int count = getClipboardCount();
    char dst[MAX_PATH_LEN];
    int conflicts = 0;
    for (int i = 0; i < count && conflicts < cap; i++) {
        const char *src = getClipboardPath(i);
        if (pastingIntoOwnDir(src)) continue;   // suffixed (copy) or no-op (move)
        joinPath(dst, MAX_PATH_LEN, destDir, getBaseName(src));
        conflicts += countTreeConflicts(src, dst, cap - conflicts);
    }
    return conflicts;
}

void runPaste(void)
{
    int copying = (getClipboardMode() == CLIP_COPY);
    int count   = getClipboardCount();

    // phase 1: total bytes for the percentage - reuse known sizes, walk only
    // the items whose size is a lower bound.
    uint64_t total = 0;
    for (int i = 0; i < count && !isCancelRequested(); i++)
        total += itemBytes(i);
    setTotalBytes(total);

    // phase 2: carry each entry across, reporting bytes and honouring cancel.
    char dst[MAX_PATH_LEN];
    for (int i = 0; i < count; i++) {
        if (isCancelRequested()) break;
        const char *src  = getClipboardPath(i);
        const char *name = getBaseName(src);

        if (copying) {
            // a copy back into the source dir duplicates beside the original.
            if (pastingIntoOwnDir(src)) {
                uniqueDest(dst, MAX_PATH_LEN, destDir, name, isDir(src));
                int r = copyTreeProgress(src, dst, copyBuf, COPY_BUF_SIZE, addProcessedBytes, isCancelRequested);
                if (r == 1) { deleteTree(dst, NULL); break; }  // cancelled: drop partial
                continue;
            }

            joinPath(dst, MAX_PATH_LEN, destDir, name);
            if (!fileExists(dst)) {
                int r = copyTreeProgress(src, dst, copyBuf, COPY_BUF_SIZE, addProcessedBytes, isCancelRequested);
                if (r == 1) { deleteTree(dst, NULL); break; }
            } else {
                // merge into the existing destination, replacing or keeping leaves.
                int r = mergeTreeProgress(src, dst, replaceOnConflict, copyBuf, COPY_BUF_SIZE, addProcessedBytes, isCancelRequested);
                if (r == 1) break;  // partial merge: leave what landed in place
            }
            continue;
        }

        // move
        joinPath(dst, MAX_PATH_LEN, destDir, name);
        if (strEq(src, dst)) continue;                   // moving into own dir: no-op

        uint64_t sz = isClipboardSizeExact(i) ? getClipboardSize(i) : measureTree(src, isCancelRequested);

        if (!fileExists(dst)) {
            // brand-new destination: an instant same-volume rename when possible.
            if (cellFsRename(src, dst) == CELL_FS_SUCCEEDED) { addProcessedBytes(sz); continue; }
            // cross-volume: copy then delete the source, but only once the copy has
            // fully landed. on cancel mid-copy, remove the partial dest and leave
            // the source intact so nothing is lost.
            int r = copyTreeProgress(src, dst, copyBuf, COPY_BUF_SIZE, addProcessedBytes, isCancelRequested);
            if (r == 1) { deleteTree(dst, NULL); break; }
            if (r == 0) deleteTree(src, NULL);
            continue;
        }

        // destination exists: merge the move in (copy honouring the policy), then
        // drop the source once it has fully landed. on cancel, keep the source.
        int r = mergeTreeProgress(src, dst, replaceOnConflict, copyBuf, COPY_BUF_SIZE, addProcessedBytes, isCancelRequested);
        if (r == 1) break;            // partial: source kept intact
        if (r == 0) deleteTree(src, NULL);
    }
}
