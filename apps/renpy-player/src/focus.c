#include "focus.h"

#define FOCUS_MAX 128
#define CROSSRANGE 1024   // config.focus_crossrange_penalty

static struct { int id, x, y, w, h; } item[FOCUS_MAX];
static int count;
static int focusedId = -1;

void clearFocus(void)      { count = 0; focusedId = -1; }
void beginFocusFrame(void) { count = 0; }   // the focusables are re-registered; the focus id persists
int  getFocusId(void)         { return focusedId; }
int  isFocused(int id)       { return focusedId == id; }
void setFocus(int id)      { focusedId = id; }

void addFocus(int id, int x, int y, int w, int h)
{
    if (count >= FOCUS_MAX) return;
    item[count].id = id; item[count].x = x; item[count].y = y; item[count].w = w; item[count].h = h;
    count++;
    if (focusedId < 0) focusedId = id;   // nothing focused yet -> the first focusable
}

static int indexOf(int id) { for (int i = 0; i < count; i++) if (item[i].id == id) return i; return -1; }

// Mouse focus: the focusable whose rendered rectangle contains the pointer (Ren'Py's focus.py focuses
// the widget under the mouse by rect intersection). Topmost wins -- last-registered is drawn on top,
// so scan back-to-front. Returns the id, or -1 if the pointer is over no focusable.
int focusAt(int px, int py)
{
    for (int i = count - 1; i >= 0; i--)
        if (px >= item[i].x && px < item[i].x + item[i].w &&
            py >= item[i].y && py < item[i].y + item[i].h)
            return item[i].id;
    return -1;
}

// squared distance between two points, with a per-axis fudge multiplier (points_dist).
static long long pointDist(long long x0, long long y0, long long x1, long long y1, long long xf, long long yf)
{
    long long dx = (x0 - x1) * xf, dy = (y0 - y1) * yf;
    return dx * dx + dy * dy;
}

static int rangesOverlap(int a0, int a1, int b0, int b1)
{
    return (b0 <= a0 && a1 <= b1) || (a0 <= b0 && b1 <= a1) || (a0 <= b0 && a1 <= b1 && b0 <= a1) || (b0 <= a0 && b1 <= a1 && a0 <= b1);
}

void moveFocus(int dx, int dy)
{
    int ci = indexOf(focusedId);
    if (count == 0) return;
    if (ci < 0) { focusedId = item[0].id; return; }

    int cx0 = item[ci].x, cy0 = item[ci].y, cx1 = cx0 + item[ci].w, cy1 = cy0 + item[ci].h;
    long long best = -1;
    int bestId = -1;

    for (int i = 0; i < count; i++)
    {
        if (i == ci) continue;
        int x0 = item[i].x, y0 = item[i].y, x1 = x0 + item[i].w, y1 = y0 + item[i].h;
        long long dist;

        if (dy != 0)   // vertical move: horiz_line_dist between the from/to horizontal edges
        {
            if (dy > 0 && !(cy1 <= y0)) continue;   // candidate must be below
            if (dy < 0 && !(y1 <= cy0)) continue;   // candidate must be above
            int fy = (dy > 0) ? cy1 : cy0;          // current bottom (down) / top (up)
            int ty = (dy > 0) ? y0 : y1;            // candidate top (down) / bottom (up)
            if (rangesOverlap(cx0, cx1, x0, x1)) dist = (long long)(fy - ty) * (fy - ty);
            else if (cx1 <= x0)                  dist = pointDist(cx1, fy, x0, ty, CROSSRANGE, 1);
            else                                 dist = pointDist(cx0, fy, x1, ty, CROSSRANGE, 1);
        }
        else           // horizontal move: verti_line_dist between the from/to vertical edges
        {
            if (dx > 0 && !(cx1 <= x0)) continue;   // candidate must be to the right
            if (dx < 0 && !(x1 <= cx0)) continue;   // candidate must be to the left
            int fx = (dx > 0) ? cx1 : cx0;
            int tx = (dx > 0) ? x0 : x1;
            if (rangesOverlap(cy0, cy1, y0, y1)) dist = (long long)(fx - tx) * (fx - tx);
            else if (cy1 <= y0)                  dist = pointDist(fx, cy1, tx, y0, 1, CROSSRANGE);
            else                                 dist = pointDist(fx, cy0, tx, y1, 1, CROSSRANGE);
        }

        if (best < 0 || dist < best) { best = dist; bestId = item[i].id; }
    }

    if (bestId >= 0) focusedId = bestId;
}
