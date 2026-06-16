#pragma once

#include "font.h"   // Font, TextTexture

#define NVL_MAX 48   // most NVL lines kept on a page (older ones scroll off; also the rollback cap)

// The dialogue / NVL / menu text layer. Builds the on-screen game text into textures and reveals
// it with the typewriter; owns those textures, the NVL page, and the typewriter. The screen feeds
// it the current line (or menu, or end message) plus the letterboxed content rect, then asks it to
// draw. `typing` types the line out (1) or shows it whole (0, e.g. an instant re-show).

void initSay(Font *font);   // inject the shared font (opened and owned by the screen)
void initSayIngame(Font *font);   // inject the in-game chat font (NULL => fall back to the shared font)

// Show one line. `nvl` = an NVL line (accumulates on the page); `freshPage` starts a new NVL page
// (the first NVL line after ADV, or after `nvl clear`). `ingame` = a chat-box line (small fixed box).
void showSayLine(int cx, int cy, int cw, int ch, const char *who, const char *what, int nvl, int ingame, int freshPage, int typing);

// Rebuild the whole NVL page (rollback restores it from history) and show it.
void showSayNvlPage(int cw, const char *const *who, const char *const *what, int count, int typing);

void showSayBlank(void);   // no-dialogue frame (rolled-back pause): draw no textbox, just the scene
void showSayMenu(int cw, const char *const *captions, int count, int selected);   // build the choice list
void showSayEnd(int cw, const char *message);                                     // "The End." / error text
void clearSayNvl(void);                                                           // `nvl clear` / scene change

void tickSay(void);            // advance the typewriter (each M_SAY frame)
int  isSayTypingDone(void);      // is the current line fully revealed?
void completeSayTyping(void);  // reveal the whole line now (first advance press)

void drawSayLine(int cx, int cy, int cw, int ch);                            // ADV textbox or NVL page
void drawSayMenu(int cx, int cy, int cw, int ch, int selected, int count);   // the choice list
void drawSayEnd(int cx, int cy, int cw, int ch);                             // end / error text
void freeSay(void);
