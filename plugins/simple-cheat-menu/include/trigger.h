#pragma once

// the cheat-menu's pad-trigger + capture state machine (trigger.c). the PS button
// is the trigger, split by hold length: a SHORT press opens the overlay and captures
// input (vsh libpad blinded, raw kernel reads take over); circle drops back to the
// in-game XMB, PS resumes the game. a LONG press (PS still held as the overlay opens)
// is the quit/power menu and is left alone. see trigger.c for the full model.

// spawn the trigger thread + the cheat job worker. call once from _start (returns
// immediately — the threads do all the work).
void startCheatMenuThreads(void);
