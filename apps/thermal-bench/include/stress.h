#pragma once

// stress - the load the bench puts on the console. the cpu and the graphics
// chip have their own dial so a run can heat one at a time, and each dial runs
// the same four steps.
//
// cpu load is busy worker threads running below the ui's priority, so the
// overlay stays responsive while the cell is pinned. each step adds a different
// part of the chip rather than another thread: the vector unit, then the scalar
// floating-point unit, then traffic to main memory. the ppu is only two hardware
// threads, so a third busy thread would share time with the other two instead of
// adding anything, while those three parts each draw their own power.
//
// gpu load is overlapping translucent geometry (blending, which is fill rate)
// plus layers of a large noisy texture drawn smaller than it is, which is what
// puts the texture units and video memory to work. flat geometry alone leaves
// both idle.

typedef enum LoadLevel {
   LOAD_OFF,
   LOAD_LIGHT,
   LOAD_MEDIUM,
   LOAD_FULL,
   LOAD_LEVEL_COUNT
} LoadLevel;

// the spus are six co-processors on the same chip as the ppu, and in a real
// game they produce most of the heat - without them a "full" burn is not full.
#define MAX_SPU_THREADS 6

typedef struct LoadState {
   LoadLevel cpuLevel;   // also drives the spu load
   LoadLevel gpuLevel;
   int spuThreadCount;   // how many spus the cell dial actually got, 0..MAX_SPU_THREADS
} LoadState;

// allocates the buffer the memory loads move around and the texture the gpu load
// samples. call once before any load is switched on.
void initStress(void);

const char *getLoadLevelName(LoadLevel level);
const LoadState *getLoadState(void);

// each dial steps to its next level and wraps back to off past the last one.
void stepCpuLoad(void);
void stepGpuLoad(void);
void stepBothLoads(void);   // both dials to one past whichever is currently higher

void setLoadOff(void);   // how the safety cutoff drops everything

// drops every load to nothing without forgetting the chosen levels, and puts them
// back. used while the XMB is open over the app.
void setStressSuspended(int suspended);

// stops every worker for good and frees what initStress took; call before the app exits.
void stopStress(void);

// the full-screen geometry that heats the graphics chip; also advances its own
// animation, so there is nothing to call from the update path.
void drawStressCanvas(void);
