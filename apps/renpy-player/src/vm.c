#include "vm.h"

#include <stdlib.h>   // atof
#include <string.h>   // strstr, strchr, strncmp

#include "vars.h"     // isCondTrue, evalExpr, setVar, applyAssign, Value
#include "config.h"   // MAX_CHOICES

#define CALL_STACK VM_CALL_STACK_MAX
#define INSTR_BUDGET 5000000   // runaway-loop guard

static RbcProgram *program;
static int         pc;
static int         callStack[CALL_STACK];
static int         sp;
static int         initMode;   // 1 while running the __init__ prologue (Return/End -> stop, not boot)

void initVm(RbcProgram *prog) { program = prog; }
RbcProgram *getProgram(void)  { return program; }
void startVm(int address)     { sp = 0; pc = address; }
void gotoVm(int address)      { pc = address; }

// Run the __init__ prologue (caller has startVm'd its address) to its Return, applying side effects
// (var assigns, overlay activation, defaults) WITHOUT triggering boot/endProgram. Init has no Say/Menu.
void runVmInit(void) { initMode = 1; runVm(); initMode = 0; }

void getVmState(int *outPc, int *outSp, int *outStack)
{
    *outPc = pc; *outSp = sp;
    for (int i = 0; i < sp; i++) outStack[i] = callStack[i];
}

void setVmState(int newPc, int newSp, const int *stack)
{
    if (newSp < 0) newSp = 0;
    if (newSp > CALL_STACK) newSp = CALL_STACK;
    pc = newPc; sp = newSp;
    for (int i = 0; i < sp; i++) callStack[i] = stack[i];
}

// Collect the choices following a MenuStart: keep those whose condition is true, but if that
// filters out everything show them all, so a menu never dead-ends.
static int collectChoices(int rawCount, const char **captions, int *targets)
{
    int count = 0;
    for (int i = 0; i < rawCount && count < MAX_CHOICES; i++)
    {
        RbcInstr *choice = &program->code[pc + 1 + i];
        if (!isCondTrue(program, choice->b)) continue;
        captions[count] = getRbcStr(program, choice->a);
        targets[count]  = choice->c;
        count++;
    }
    if (count == 0)
        for (int i = 0; i < rawCount && count < MAX_CHOICES; i++)
        {
            RbcInstr *choice = &program->code[pc + 1 + i];
            captions[count] = getRbcStr(program, choice->a);
            targets[count]  = choice->c;
            count++;
        }
    return count;
}

void runVm(void)
{
    int steps = 0;
    for (;;)
    {
        if (++steps > INSTR_BUDGET)                         { failVm("instruction budget exceeded (loop?)"); return; }
        if (!program || pc < 0 || pc >= program->instrCount) { if (initMode) return; endProgram(); return; }

        RbcInstr *in = &program->code[pc];
        switch (in->op)
        {
            case RBC_LABEL: pc++; break;

            case RBC_SAY:
                beginSay(in->a >= 0 ? getRbcStr(program, in->a) : NULL, getRbcStr(program, in->b), in->c == 1, in->c == 2);
                pc++;
                return;

            case RBC_SCENE:
                applyScene(getRbcStr(program, in->a));
                pc++;
                break;

            case RBC_SHOW:
                showImage(getRbcStr(program, in->a), in->b >= 0 ? getRbcStr(program, in->b) : "", in->c);
                pc++;
                break;

            case RBC_HIDE:
                hideImage(getRbcStr(program, in->a));
                pc++;
                break;

            case RBC_WITH:
            {
                const char *name = in->a >= 0 ? getRbcStr(program, in->a) : "";
                pc++;   // resume past the With once the transition (or instant skip) finishes
                if (beginTransition(name)) return;
                break;
            }

            case RBC_JUMP:
                if (in->a < 0)
                {
                    // Unresolved target: it may be an engine-generated screen label (load_screen,
                    // save_screen, ...) the game jumps to. Let the screen route it to its own menu; if
                    // it doesn't handle it, the jump is inert (skip past it).
                    if (in->b >= 0 && jumpToEngineScreen(getRbcStr(program, in->b))) return;
                    pc++;
                    break;
                }
                pc = in->a;
                break;
            case RBC_CALL:   if (sp < CALL_STACK) callStack[sp++] = pc + 1; pc = in->a; break;
            case RBC_RETURN: if (sp > 0) pc = callStack[--sp]; else { if (initMode) return; endProgram(); return; } break;

            case RBC_MENUSTART:
            {
                const char *captions[MAX_CHOICES];
                int targets[MAX_CHOICES];
                int n = collectChoices(in->a, captions, targets);
                const char *caption = (in->b >= 0) ? getRbcStr(program, in->b) : NULL;   // menu caption -> dialogue box
                beginMenu(caption, captions, targets, n);
                return;
            }

            case RBC_IFFALSEGOTO:
                if (isCondTrue(program, in->a)) pc++;             // condition true -> run the block
                else pc = (in->c >= 0) ? in->c : pc + 1;        // false -> jump past it
                break;

            case RBC_PYEXEC:
            {
                const char *code = getRbcStr(program, in->a);
                pc++;
                if (runPyExec(code)) return;
                break;
            }

            case RBC_USER:
                runUserLine(getRbcStr(program, in->a));
                pc++;
                break;

            case RBC_DEFAULT:
                if (in->c >= 0) { Value value; if (evalExpr(program, in->c, &value)) setVar(getRbcStr(program, in->a), &value, 1); }
                pc++;
                break;

            case RBC_ASSIGN: applyAssign(program, in); pc++; break;

            case RBC_IMAGEMAP:
                pc++;   // resume past the imagemap once a hotspot is chosen (the var is then set)
                beginImageMap(getRbcImageMap(program, in->b), in->a >= 0 ? getRbcStr(program, in->a) : "");
                return;

            case RBC_OVERLAY_SHOW: showOverlay(getRbcStr(program, in->a)); pc++; break;   // non-yielding
            case RBC_OVERLAY_HIDE: hideOverlay(getRbcStr(program, in->a)); pc++; break;

            case RBC_CHOICE:  pc++; break;   // only reached if skipped into; ignore
            case RBC_MENUEND: pc++; break;
            case RBC_IMAGE:   pc++; break;
            case RBC_PAUSE:   pc++; break;
            case RBC_NOP:     pc++; break;
            case RBC_END:     if (initMode) return; endProgram(); return;
            default:          pc++; break;
        }
    }
}
