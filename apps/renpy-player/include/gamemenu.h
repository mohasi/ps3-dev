#pragma once

#include "font.h"   // Font

// The classic-theme in-game menu (config.game_menu): the navigation column (Return, Preferences,
// Save, Load, Main Menu, Quit) plus the engine yes/no prompt that Main Menu / Quit run before
// acting. Opened in-game (right-click -> Triangle). The screen drives the chosen action; the file
// picker (Save/Load) and Preferences land as later steps.

typedef enum {
    GM_NONE = 0,    // nothing chosen yet (still navigating)
    GM_RETURN,      // resume the game
    GM_PREFERENCES, // open the preferences screen
    GM_SAVE,        // open the save file picker
    GM_LOAD,        // open the load file picker
    GM_MAINMENU,    // return to the title (after the engine yes/no prompt)
    GM_QUIT,        // quit (after the engine yes/no prompt)
    GM_DO_SAVE,     // write the focused slot (after the overwrite prompt, if it was occupied)
    GM_DO_LOAD      // load the focused slot (after the load prompt)
} GmAction;

void        initGameMenu(Font *font);               // inject the shared font (opened by the screen)
void        enterGameMenu(void);                    // open the menu in-game (lands on Save; main_menu = False)
void        enterGameMenuOn(const char *engineScreen);  // open in-game on a specific sub-screen by its engine label (save_screen/load_screen/preferences_screen)
void        enterGameMenuFromTitle(void);           // open from the title for "Load Game" (Load screen; main_menu = True)
void        drawGameMenu(int cx, int cy, int cw, int ch);
GmAction    updateGameMenu(int curVisible, int curX, int curY);   // poll pad + virtual cursor -> action (GM_NONE while navigating)
const char *getGameMenuSelectedSlot(void);             // the slot picked for GM_DO_SAVE / GM_DO_LOAD ("1".."a5")
void        refreshGameMenuSlots(void);             // re-read the save folder (after a save) -> slot labels
void        freeGameMenu(void);
