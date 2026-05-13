#pragma once

// file-list - scrollable directory listing widget

#include "font.h"
#include "gfx.h"
#include "ui/breadcrumb.h"

#define FILE_LIST_PAGE_SIZE  9

void initFileList(Font *font, GfxTexture spritesheet, int x, int y, int maxWidth, int rowHeight, int fontSize, uint32_t color, Breadcrumb *bc);
void termFileList(void);
void updateFileList(void);
void drawFileList(void);
