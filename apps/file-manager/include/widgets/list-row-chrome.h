#pragma once

// list-row-chrome - the flat row chrome shared by the file list and the search results: a hairline
// between rows, and the selection bar for the active row. same 1080p geometry + active-theme colours
// in both, so it lives here rather than being copied per widget.

#include "gfx.h"
#include "theme.h"

static inline void drawListRowSeparator(int rowY)
{
   fillGfxRectangle(47, rowY - 1, 1884 - 47, 2, activeTheme->separator);
}

static inline void drawListRowHighlight(int rowY, int rowHeight)
{
   drawGfxBox(42, rowY, 1882 - 47, rowHeight, activeTheme->borderThickness, activeTheme->highlightFill, activeTheme->highlightBorder);
}
