#pragma once

// stress - the load the bench puts on the console. the cpu and the graphics
// chip have their own dial so a run can heat one at a time, and each dial runs
// the same four steps.
//
// cpu load is busy worker threads running below the ui's priority, so the
// overlay stays responsive while the cell is pinned. gpu load is heavy
// overlapping translucent geometry filling the whole screen - fill rate with
// blending is what actually heats the rsx.

#define LOAD_LEVEL_COUNT 4

// the spus are six co-processors on the same chip as the ppu, and in a real
// game they produce most of the heat - without them a "full" burn is not full.
#define MAX_SPU_THREADS 6

typedef struct LoadState {
   int cpuLevel;   // 0..3, also drives the spu load
   int spuLevel;
   int gpuLevel;
} LoadState;

const char *getLoadLevelName(int level);
const LoadState *getLoadState(void);

void setCpuLoad(int level);
void setGpuLoad(int level);

// drops every load to nothing without forgetting the chosen levels, and puts
// them back. used while the XMB is open over the app.
void suspendStress(void);
void resumeStress(void);

// stops every worker for good; call before the app exits.
void stopStress(void);

// the full-screen geometry that heats the graphics chip; also advances its own
// animation, so there is nothing to call from the update path.
void drawStressCanvas(void);
