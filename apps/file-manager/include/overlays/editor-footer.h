#pragma once

// editor-footer - the Edit / Save / Exit button row along the bottom of the text editor and the hex
// viewer. Both build, draw, free and recolour the identical row; only the per-frame enabled states
// differ, which each overlay passes to setEditorFooterStates. Each overlay owns one EditorFooter.

#include "ui/button.h"
#include "ui/image.h"
#include "ui/label.h"
#include "ui/console-glyphs.h"
#include "theme.h"

typedef struct { Button edit, save, exit; } EditorFooter;

#define EDITOR_FOOTER_X          51
#define EDITOR_FOOTER_Y        1018
#define EDITOR_FOOTER_TEXT_SIZE  20
#define EDITOR_FOOTER_ICON_H     32   // console button glyphs are scaled to this height, aspect preserved
#define EDITOR_FOOTER_GAP        50

// builds one button (glyph + label), docks it at *x on the footer row, then advances *x past it.
static inline void initEditorFooterButton(Button *button, ConsoleGlyph glyph, const char *text, ButtonState state, Font *font, int *x)
{
   Image icon;
   Label label;
   initGlyphIcon(&icon, glyph, EDITOR_FOOTER_ICON_H);
   initLabel(&label, font, 0, 0, AUTO, AUTO, EDITOR_FOOTER_TEXT_SIZE, activeTheme->textPrimary, TEXT_NOWRAP, text);
   initButton(button, icon, label, state);
   moveButton(button, *x, EDITOR_FOOTER_Y);
   *x += getButtonWidth(button) + EDITOR_FOOTER_GAP;
}

static inline void initEditorFooter(EditorFooter *footer, Font *font)
{
   int x = EDITOR_FOOTER_X;
   initEditorFooterButton(&footer->edit, GLYPH_CROSS,  "Edit", BUTTON_ENABLED,  font, &x);
   initEditorFooterButton(&footer->save, GLYPH_START,  "Save", BUTTON_DISABLED, font, &x);
   initEditorFooterButton(&footer->exit, GLYPH_CIRCLE, "Exit", BUTTON_ENABLED,  font, &x);
}

static inline void setEditorFooterStates(EditorFooter *footer, ButtonState edit, ButtonState save, ButtonState exit)
{
   setButtonState(&footer->edit, edit);
   setButtonState(&footer->save, save);
   setButtonState(&footer->exit, exit);
}

static inline void drawEditorFooter(EditorFooter *footer)
{
   drawButton(&footer->edit);
   drawButton(&footer->save);
   drawButton(&footer->exit);
}

static inline void rethemeEditorFooter(EditorFooter *footer)
{
   setLabelColor(&footer->edit.label, activeTheme->textPrimary);
   setLabelColor(&footer->save.label, activeTheme->textPrimary);
   setLabelColor(&footer->exit.label, activeTheme->textPrimary);
}

static inline void freeEditorFooter(EditorFooter *footer)
{
   freeButton(&footer->edit);
   freeButton(&footer->save);
   freeButton(&footer->exit);
}
