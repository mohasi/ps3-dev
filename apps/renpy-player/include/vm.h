#pragma once

#include "rbc.h"

// The Ren'Py bytecode interpreter. It owns the program counter and call stack and decodes one
// instruction at a time; every visible effect is delegated to a host hook (below), so the VM
// stays a pure decoder with no knowledge of how scenes, text, or menus are drawn or stored.

#define VM_CALL_STACK_MAX 64        // call-stack depth (sizes save buffers)

void initVm(RbcProgram *program);   // inject the loaded program (once, after parsing)
RbcProgram *getProgram(void);       // the loaded program (for menu screens that read baked imagemaps)
void startVm(int address);          // reset the call stack and begin at `address`
void gotoVm(int address);           // set the program counter (e.g. a chosen menu target)
void runVm(void);                   // execute from the current pc until a hook yields or the program ends
void runVmInit(void);               // run the __init__ prologue to its Return (no boot/endProgram)

// Save/restore the full execution position (pc + call stack), for the game-menu save/load. `stack`
// must hold VM_CALL_STACK_MAX ints. getVmState reports the live pc (already past the current Say),
// so a restored game resumes at the next statement while the saved line is re-shown from its frame.
void getVmState(int *pc, int *sp, int *stack);
void setVmState(int pc, int sp, const int *stack);

// ---- host hooks: implemented by the screen, called by the VM as it executes ----
// A hook that can pause execution returns 1 to make the VM yield (stop and let the screen
// present something) or 0 to keep running the next instruction.

void beginSay(const char *who, const char *what, int nvl, int ingame);        // show a line (always yields); ingame = chat-box line
int  jumpToEngineScreen(const char *name);   // route an unresolved jump to an engine screen (load_screen/...) to a menu; 1 = handled (yield)
void applyScene(const char *name);                                           // replace the background
void showImage(const char *name, const char *at, int atlId);                 // show a sprite (atlId<0 = no ATL)
void hideImage(const char *name);                                            // hide a sprite
int  beginTransition(const char *name);                                      // 1 = a transition is playing
void beginMenu(const char *caption, const char *const *captions, const int *targets, int count);  // caption (narrator line, NULL=none) + choices; always yields
void beginImageMap(const RbcImageMap *im, const char *resultVar);            // imagemap menu (always yields)
void showOverlay(const char *name);                                          // activate a HUD overlay
void hideOverlay(const char *name);                                          // deactivate a HUD overlay
int  runPyExec(const char *code);                                            // raw python; 1 = a pause started
void runUserLine(const char *line);                                          // `nvl clear`, `play`/`stop`, ...
void endProgram(void);                                                       // program ended (-> menu or "The End.")
void failVm(const char *message);                                            // unrecoverable VM error
