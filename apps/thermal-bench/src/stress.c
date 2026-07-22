// stress - the load dials, the cpu worker threads and the full-screen canvas.
// see stress.h for why each load is shaped the way it is.
#include <math.h>
#include <sys/spu_initialize.h>
#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>

#include "stress.h"
#include "gfx.h"
#include "colors.h"
#include "thread.h"
#include "dbg.h"

// how hard each of the four steps pushes. cpu is worker threads; gpu is large
// translucent triangles, tuned so the heaviest step keeps the overlay readable.
static const char *LEVEL_NAMES[LOAD_LEVEL_COUNT] = { "off", "light", "medium", "full" };
static const int CPU_WORKERS[LOAD_LEVEL_COUNT]   = { 0, 1, 3, 6 };
static const int SPU_THREADS[LOAD_LEVEL_COUNT]   = { 0, 2, 4, 6 };
static const int GPU_TRIANGLES[LOAD_LEVEL_COUNT] = { 0, 80, 220, 400 };

#define SPU_GROUP_PRIORITY 100

extern const char _binary_spuburn_elf_start[];   // the embedded spu program

#define SPIN_PER_FRAME 0.01f

// workers run well below the ui thread (higher number = lower priority), so a
// pinned cell still leaves the overlay smooth.
#define WORKER_PRIORITY   2000
#define WORKER_STACK_SIZE 0x4000
#define MAX_CPU_WORKERS   6
#define BURN_BATCH        200000   // iterations between stop-flag checks

static LoadState load;
static int suspended;

static float spin;
static sys_ppu_thread_t workers[MAX_CPU_WORKERS];
static int workerCount;
static volatile int stopWorkers;

static sys_spu_thread_group_t spuGroup;
static sys_spu_image_t spuImage;
static int spuImageOpen;
static int spuThreadCount;

const char *getLoadLevelName(int level)
{
   return (unsigned)level < LOAD_LEVEL_COUNT ? LEVEL_NAMES[level] : "?";
}

const LoadState *getLoadState(void) { return &load; }

// busy loop. four independent chains keep the pipeline full instead of stalling
// on the previous result, and the volatile sink stops the compiler folding the
// whole thing away. the arithmetic stays in range forever rather than drifting
// to infinity.
static void burnCpu(uint64_t seed)
{
   static volatile double sink;
   double a = (double)seed + 1.0, b = a + 0.25, c = a + 0.5, d = a + 0.75;

   while (!stopWorkers)
   {
      for (int i = 0; i < BURN_BATCH; i++)
      {
         a = a * 1.0000001 + 0.5 - a * 0.0000001;
         b = b * 1.0000001 + 0.5 - b * 0.0000001;
         c = c * 1.0000001 + 0.5 - c * 0.0000001;
         d = d * 1.0000001 + 0.5 - d * 0.0000001;
      }
      sink = a + b + c + d;
   }
   exitThread();
}

static void stopCpuWorkers(void)
{
   if (workerCount == 0) return;
   stopWorkers = 1;
   for (int worker = 0; worker < workerCount; worker++) joinThread(workers[worker]);
   workerCount = 0;
   stopWorkers = 0;
}

static void startCpuWorkers(int count)
{
   if (count > MAX_CPU_WORKERS) count = MAX_CPU_WORKERS;
   while (workerCount < count)
   {
      if (spawnJoinableThread(&workers[workerCount], burnCpu, (uint64_t)workerCount, WORKER_PRIORITY, WORKER_STACK_SIZE, "bench-burn") != 0)
      {
         logError("[bench] could not start cpu worker %d\n", workerCount);
         return;
      }
      workerCount++;
   }
}

// spu load. the burn program never returns, so a running group is stopped by
// terminating it outright - no shutdown handshake to get wrong.
static void stopSpuThreads(void)
{
   if (spuThreadCount == 0) return;

   int cause = 0, status = 0;
   sys_spu_thread_group_terminate(spuGroup, 0);
   sys_spu_thread_group_join(spuGroup, &cause, &status);
   sys_spu_thread_group_destroy(spuGroup);
   spuThreadCount = 0;
}

static int startSpuThreads(int count)
{
   if (count > MAX_SPU_THREADS) count = MAX_SPU_THREADS;

   if (!spuImageOpen)
   {
      if (sys_spu_initialize(MAX_SPU_THREADS, 0) != CELL_OK) { logError("[bench] sys_spu_initialize failed\n"); return 0; }
      if (sys_spu_image_import(&spuImage, _binary_spuburn_elf_start, SYS_SPU_IMAGE_DIRECT) != CELL_OK)
      {
         logError("[bench] embedded spu program would not load\n");
         return 0;
      }
      spuImageOpen = 1;
   }

   sys_spu_thread_group_attribute_t groupAttribute;
   sys_spu_thread_group_attribute_initialize(groupAttribute);
   sys_spu_thread_group_attribute_name(groupAttribute, "bench-spu-burn");
   if (sys_spu_thread_group_create(&spuGroup, count, SPU_GROUP_PRIORITY, &groupAttribute) != CELL_OK)
   {
      logError("[bench] could not create a group of %d spu threads\n", count);
      return 0;
   }

   sys_spu_thread_attribute_t threadAttribute;
   sys_spu_thread_attribute_initialize(threadAttribute);
   sys_spu_thread_attribute_name(threadAttribute, "bench-spu");
   for (int spu = 0; spu < count; spu++)
   {
      sys_spu_thread_t thread;
      sys_spu_thread_argument_t arguments = { 0 };
      arguments.arg1 = SYS_SPU_THREAD_ARGUMENT_LET_32((unsigned)spu);
      if (sys_spu_thread_initialize(&thread, spuGroup, spu, &spuImage, &threadAttribute, &arguments) != CELL_OK)
      {
         logError("[bench] could not start spu thread %d\n", spu);
         sys_spu_thread_group_destroy(spuGroup);
         return 0;
      }
   }

   if (sys_spu_thread_group_start(spuGroup) != CELL_OK)
   {
      logError("[bench] could not start the spu thread group\n");
      sys_spu_thread_group_destroy(spuGroup);
      return 0;
   }
   spuThreadCount = count;
   return count;
}

static void applySpuLoad(void)
{
   int wanted = suspended ? 0 : SPU_THREADS[load.cpuLevel];
   if (wanted == spuThreadCount) return;

   stopSpuThreads();
   // other parts of the system may already hold some spus; take what we can get
   while (wanted > 0 && startSpuThreads(wanted) == 0) wanted--;
   load.spuLevel = spuThreadCount;
}

// the frame cap comes off only while there is gpu load, so the rsx runs flat
// out during a burn and idles politely the rest of the time.
static void applyGpuLoad(void)
{
   int level = suspended ? 0 : load.gpuLevel;
   setGfxVsync(level > 0 ? GFX_VSYNC_OFF : GFX_VSYNC_ON);
}

static void applyCpuLoad(void)
{
   int wanted = suspended ? 0 : CPU_WORKERS[load.cpuLevel];
   if (wanted == workerCount) return;
   if (wanted < workerCount) stopCpuWorkers();
   startCpuWorkers(wanted);
}

void setCpuLoad(int level)
{
   load.cpuLevel = (level + LOAD_LEVEL_COUNT) % LOAD_LEVEL_COUNT;
   applyCpuLoad();
   applySpuLoad();   // the spus live on the same chip, so they follow the cpu dial
   logInfo("[bench] cpu load '%s': %d ppu workers, %d spu threads\n", getLoadLevelName(load.cpuLevel), workerCount, spuThreadCount);
}

void setGpuLoad(int level)
{
   load.gpuLevel = (level + LOAD_LEVEL_COUNT) % LOAD_LEVEL_COUNT;
   applyGpuLoad();
   logInfo("[bench] gpu load '%s': %d triangles\n", getLoadLevelName(load.gpuLevel), GPU_TRIANGLES[load.gpuLevel]);
}

void suspendStress(void)
{
   if (suspended) return;
   suspended = 1;
   applyCpuLoad();
   applySpuLoad();
   applyGpuLoad();
   logInfo("[bench] load suspended (xmb open)\n");
}

void resumeStress(void)
{
   if (!suspended) return;
   suspended = 0;
   applyCpuLoad();
   applySpuLoad();
   applyGpuLoad();
   logInfo("[bench] load resumed\n");
}

void stopStress(void)
{
   suspended = 1;
   stopCpuWorkers();
   stopSpuThreads();
   if (spuImageOpen) { sys_spu_image_close(&spuImage); spuImageOpen = 0; }
}

void drawStressCanvas(void)
{
   spin += SPIN_PER_FRAME;

   int triangles = suspended ? 0 : GPU_TRIANGLES[load.gpuLevel];
   if (triangles == 0) return;

   int screenW = getGfxScreenWidth(), screenH = getGfxScreenHeight();
   float centerX = screenW / 2.0f, centerY = screenH / 2.0f;
   float radius = (screenW < screenH ? screenW : screenH) * 0.75f;

   // low alpha so overlapping triangles build up instead of hiding each other
   uint32_t inner = (COLOR_INDIGO_600 & 0x00FFFFFF) | 0x30000000;
   uint32_t left  = (COLOR_ROSE_600   & 0x00FFFFFF) | 0x30000000;
   uint32_t right = (COLOR_SLATE_800  & 0x00FFFFFF) | 0x30000000;

   for (int t = 0; t < triangles; t++)
   {
      float base = spin + t * 6.2831853f / triangles;
      float x0 = centerX + radius * cosf(base);
      float y0 = centerY + radius * sinf(base);
      float x1 = centerX + radius * cosf(base + 2.2f);
      float y1 = centerY + radius * sinf(base + 2.2f);
      drawGfxTriangle(centerX, centerY, inner, x0, y0, left, x1, y1, right);
   }
}
