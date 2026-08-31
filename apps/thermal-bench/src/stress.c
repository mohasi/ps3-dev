// stress - the load dials, the cpu worker threads and the full-screen canvas.
// see stress.h for why each load is shaped the way it is.
#include <altivec.h>
#include <stdlib.h>
#include <math.h>
#include <sys/spu_initialize.h>
#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>

#include "stress.h"
#include "gfx.h"
#include "colors.h"
#include "thread.h"
#include "string-utilities.h"   // memSet, in place of libc
#include "dbg.h"

// how hard each of the four steps pushes.
//
// the cpu steps add a unit rather than a thread: the ppu is two hardware
// threads, so a third busy thread only shares time with the other two, while
// the vector unit, the scalar fpu and the memory path each draw their own
// power. worker 0 runs vector arithmetic, worker 1 scalar arithmetic, worker 2
// memory traffic.
//
// the gpu steps are large translucent triangles (blending, which is fill rate)
// plus layers of a big noisy texture drawn smaller than it is (texture fetch and
// video memory, which flat triangles never touch).
static const char *LEVEL_NAMES[LOAD_LEVEL_COUNT]     = { "off", "light", "medium", "full" };
static const int CPU_WORKERS[LOAD_LEVEL_COUNT]       = { 0, 1, 2, 3 };
static const int SPU_THREADS[LOAD_LEVEL_COUNT]       = { 0, 2, 4, 6 };
static const int GPU_TRIANGLES[LOAD_LEVEL_COUNT]     = { 0, 120, 320, 640 };
static const int GPU_NOISE_LAYERS[LOAD_LEVEL_COUNT]  = { 0, 4, 10, 20 };

#define SPU_GROUP_PRIORITY 100

extern const char _binary_spuburn_elf_start[];   // the embedded spu program

#define SPIN_PER_FRAME 0.01f

// workers run well below the ui thread (higher number = lower priority), so a
// pinned cell still leaves the overlay smooth.
#define WORKER_PRIORITY   2000
#define WORKER_STACK_SIZE 0x4000
#define MAX_CPU_WORKERS   3
#define BURN_BATCH        20000   // chain steps between stop-flag checks

// one buffer serves both memory loads: the cpu worker streams the whole of it,
// and each spu moves its own window of it in and out. sized past the ppu's
// 512 KB second-level cache so every line the cpu touches comes from main
// memory rather than the cache.
#define MEMORY_TRAFFIC_BYTES (4 * 1024 * 1024)
#define SPU_WINDOW_BYTES     (256 * 1024)

// big enough that a screen-sized draw of it reads a different part per pixel,
// which is what misses the texture cache
#define NOISE_TEXTURE_SIZE 1024
#define NOISE_LAYER_TINT   0x40FFFFFF   // 25% opacity, so layers build up

static LoadState currentLoad;
static int suspended;

static float spin;
static void *memoryTrafficBuffer;
static GfxTexture noiseTexture;

static sys_ppu_thread_t workers[MAX_CPU_WORKERS];
static int workerCount;
static volatile int stopWorkers;

static sys_spu_thread_group_t spuGroup;
static sys_spu_image_t spuImage;
static int spuImageOpen;
static int spuThreadCount;

const char *getLoadLevelName(LoadLevel level)
{
   return (int)level < LOAD_LEVEL_COUNT ? LEVEL_NAMES[level] : "?";
}

const LoadState *getLoadState(void) { return &currentLoad; }

static inline vector float splatFloat(float value) { return (vector float){ value, value, value, value }; }

// the chains are written out rather than looped over: a loop index would keep the
// accumulators in memory instead of registers, and keeping the pipeline full is
// the whole point of having twenty of them. the counts below say how long each
// list is, and have to be changed with it.
#define VECTOR_CHAIN_COUNT 20
#define SCALAR_CHAIN_COUNT 12

#define EACH_VECTOR_CHAIN(action) \
   action(0)  action(1)  action(2)  action(3)  action(4)  \
   action(5)  action(6)  action(7)  action(8)  action(9)  \
   action(10) action(11) action(12) action(13) action(14) \
   action(15) action(16) action(17) action(18) action(19)

#define EACH_SCALAR_CHAIN(action) \
   action(0) action(1) action(2)  action(3)  action(4)  action(5) \
   action(6) action(7) action(8)  action(9)  action(10) action(11)

static volatile vector float vectorSink;
static volatile double scalarSink;

// twenty independent multiply-add chains through the vector unit. a result is
// ten cycles behind its input, so a handful of chains would spend most of the
// time waiting; cellmark measures 23.5 GFLOPS at twenty chains against 5.4 at
// four, and it is the arithmetic that is actually running that makes heat.
static void burnVectorMath(void)
{
   vector float scale = splatFloat(0.99999988f), offset = splatFloat(1.1920929e-7f);
   vector float chain[VECTOR_CHAIN_COUNT];

#define SEED_VECTOR(index) chain[index] = splatFloat((float)(index) + 1.0f);
#define STEP_VECTOR(index) chain[index] = vec_madd(chain[index], scale, offset);
   EACH_VECTOR_CHAIN(SEED_VECTOR)

   while (!stopWorkers)
   {
      for (int step = 0; step < BURN_BATCH; step++) { EACH_VECTOR_CHAIN(STEP_VECTOR) }
      vectorSink = chain[0];
   }
#undef SEED_VECTOR
#undef STEP_VECTOR
}

// the scalar floating-point unit is a separate pipe from the vector unit, so a
// worker here adds power draw rather than competing for the same silicon.
// twelve chains, for the same reason the vector kernel has twenty.
static void burnScalarMath(void)
{
   volatile double scaleSource = 0.9999, offsetSource = 1.0e-9;
   double scale = scaleSource, offset = offsetSource;
   double chain[SCALAR_CHAIN_COUNT];

#define SEED_SCALAR(index) chain[index] = 1.0 + (index) * 0.01;
#define STEP_SCALAR(index) chain[index] = chain[index] * scale + offset;
   EACH_SCALAR_CHAIN(SEED_SCALAR)

   while (!stopWorkers)
   {
      for (int step = 0; step < BURN_BATCH; step++) { EACH_SCALAR_CHAIN(STEP_SCALAR) }
      scalarSink = chain[0];
   }
#undef SEED_SCALAR
#undef STEP_SCALAR
}

// reads and writes a buffer far larger than the cache, so every line travels to
// main memory and back. the memory interface is on the same chip as the cores.
static void burnMemoryTraffic(void)
{
   if (!memoryTrafficBuffer) { burnVectorMath(); return; }   // nothing to stream; keep the thread useful

   vector float *stream = (vector float *)memoryTrafficBuffer;
   int vectorCount = MEMORY_TRAFFIC_BYTES / (int)sizeof(vector float);
   vector float step = splatFloat(1.0f);

   while (!stopWorkers)
      for (int index = 0; index < vectorCount; index++) stream[index] = vec_add(stream[index], step);
}

static void burnCpu(uint64_t workerIndex)
{
   switch (workerIndex)
   {
      case 0:  burnVectorMath();    break;
      case 1:  burnScalarMath();    break;
      default: burnMemoryTraffic(); break;
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
      sys_ppu_thread_t *worker = &workers[workerCount];
      if (spawnJoinableThread(worker, burnCpu, workerCount, WORKER_PRIORITY, WORKER_STACK_SIZE, "bench-burn") != 0)
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
      if (sys_spu_initialize(MAX_SPU_THREADS, 0) != CELL_OK)
      {
         logError("[bench] sys_spu_initialize failed\n");
         return 0;
      }
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

      // each spu moves its own window of the shared buffer, so they load the
      // memory path together instead of fighting over one address
      uint64_t window = memoryTrafficBuffer ? (uint64_t)(uintptr_t)memoryTrafficBuffer + spu * SPU_WINDOW_BYTES : 0;
      arguments.arg1 = SYS_SPU_THREAD_ARGUMENT_LET_64((uint64_t)spu);
      arguments.arg2 = SYS_SPU_THREAD_ARGUMENT_LET_64(window);
      arguments.arg3 = SYS_SPU_THREAD_ARGUMENT_LET_64((uint64_t)SPU_WINDOW_BYTES);

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
   int wanted = suspended ? 0 : SPU_THREADS[currentLoad.cpuLevel];
   if (wanted == spuThreadCount) return;

   stopSpuThreads();
   // other parts of the system may already hold some spus; take what we can get
   while (wanted > 0 && startSpuThreads(wanted) == 0) wanted--;
   currentLoad.spuThreadCount = spuThreadCount;
}

// the frame cap comes off only while there is gpu load, so the rsx runs flat out
// during a burn and idles politely the rest of the time. the geometry that is the
// gpu load is drawn by drawStressCanvas, which reads the dial itself.
static void setVsyncForLoad(void)
{
   LoadLevel level = suspended ? LOAD_OFF : currentLoad.gpuLevel;
   setGfxVsync(level > LOAD_OFF ? GFX_VSYNC_OFF : GFX_VSYNC_ON);
}

static void applyCpuLoad(void)
{
   int wanted = suspended ? 0 : CPU_WORKERS[currentLoad.cpuLevel];
   if (wanted == workerCount) return;
   if (wanted < workerCount) stopCpuWorkers();
   startCpuWorkers(wanted);
}

static LoadLevel nextLevel(LoadLevel level) { return (level + 1) % LOAD_LEVEL_COUNT; }

static void setCpuLoad(LoadLevel level)
{
   currentLoad.cpuLevel = level;
   applyCpuLoad();
   applySpuLoad();   // the spus live on the same chip, so they follow the cpu dial
   logInfo("[bench] cpu load '%s': %d workers, %d spus\n", getLoadLevelName(level), workerCount, spuThreadCount);
}

static void setGpuLoad(LoadLevel level)
{
   currentLoad.gpuLevel = level;
   setVsyncForLoad();
   logInfo("[bench] gpu load '%s': %d triangles, %d noise layers\n", getLoadLevelName(level),
           GPU_TRIANGLES[level], GPU_NOISE_LAYERS[level]);
}

void stepCpuLoad(void) { setCpuLoad(nextLevel(currentLoad.cpuLevel)); }
void stepGpuLoad(void) { setGpuLoad(nextLevel(currentLoad.gpuLevel)); }

// one press moves both dials, so they step together from whichever is ahead
void stepBothLoads(void)
{
   LoadLevel ahead = currentLoad.cpuLevel >= currentLoad.gpuLevel ? currentLoad.cpuLevel : currentLoad.gpuLevel;
   LoadLevel next = nextLevel(ahead);
   setCpuLoad(next);
   setGpuLoad(next);
}

void setLoadOff(void)
{
   setCpuLoad(LOAD_OFF);
   setGpuLoad(LOAD_OFF);
}

void setStressSuspended(int suspend)
{
   if (suspended == suspend) return;
   suspended = suspend;
   applyCpuLoad();
   applySpuLoad();
   setVsyncForLoad();
   logInfo("[bench] load %s\n", suspend ? "suspended (xmb open)" : "resumed");
}

// random noise rather than a picture: neighbouring pixels must not share texels,
// or the texture cache answers most of the reads and the memory path stays idle.
static void createNoiseTexture(void)
{
   int pixelCount = NOISE_TEXTURE_SIZE * NOISE_TEXTURE_SIZE;
   uint32_t *pixels = (uint32_t *)malloc(pixelCount * sizeof(uint32_t));
   if (!pixels)
   {
      logError("[bench] no room for the %d x %d noise texture; gpu load will be triangles only\n",
               NOISE_TEXTURE_SIZE, NOISE_TEXTURE_SIZE);
      return;
   }

   uint32_t random = 0x12345678;
   for (int pixel = 0; pixel < pixelCount; pixel++)
   {
      random = random * 1664525u + 1013904223u;   // numerical recipes' linear congruential generator
      pixels[pixel] = 0xFF000000 | (random >> 8);
   }

   noiseTexture.offset = uploadGfxTexture(pixels, NOISE_TEXTURE_SIZE, NOISE_TEXTURE_SIZE, NOISE_TEXTURE_SIZE * 4);
   noiseTexture.w = noiseTexture.h = NOISE_TEXTURE_SIZE;
   noiseTexture.pitch = (NOISE_TEXTURE_SIZE * 4 + 63) & ~63;   // matches uploadGfxTexture
   free(pixels);

   if (noiseTexture.offset == 0) logError("[bench] noise texture would not upload; gpu load will be triangles only\n");
}

void initStress(void)
{
   // aligned to 128 bytes because that is what spu transfers want, and cleared
   // because memory nobody has written can hold values the vector unit is slow on
   memoryTrafficBuffer = memalign(128, MEMORY_TRAFFIC_BYTES);
   if (memoryTrafficBuffer) memSet(memoryTrafficBuffer, 0, MEMORY_TRAFFIC_BYTES);
   else logError("[bench] no room for the %d MB traffic buffer; memory load unavailable\n", MEMORY_TRAFFIC_BYTES >> 20);

   createNoiseTexture();
}

void stopStress(void)
{
   suspended = 1;
   stopCpuWorkers();
   stopSpuThreads();
   if (spuImageOpen) { sys_spu_image_close(&spuImage); spuImageOpen = 0; }

   freeGfxTexture(&noiseTexture);
   free(memoryTrafficBuffer);
   memoryTrafficBuffer = NULL;
}

// overlapping translucent triangles: every pixel is blended several times over,
// which is what the graphics chip's output stage does all day in a game.
static void drawTriangleFan(int triangles)
{
   if (triangles == 0) return;

   int screenWidth = getGfxScreenWidth(), screenHeight = getGfxScreenHeight();
   float centerX = screenWidth / 2.0f, centerY = screenHeight / 2.0f;
   float radius = (screenWidth < screenHeight ? screenWidth : screenHeight) * 0.75f;

   // low alpha so overlapping triangles build up instead of hiding each other
   uint32_t inner = (COLOR_INDIGO_600 & 0x00FFFFFF) | 0x30000000;
   uint32_t left  = (COLOR_ROSE_600   & 0x00FFFFFF) | 0x30000000;
   uint32_t right = (COLOR_SLATE_800  & 0x00FFFFFF) | 0x30000000;

   for (int triangle = 0; triangle < triangles; triangle++)
   {
      float base = spin + triangle * 6.2831853f / triangles;
      float x0 = centerX + radius * cosf(base);
      float y0 = centerY + radius * sinf(base);
      float x1 = centerX + radius * cosf(base + 2.2f);
      float y1 = centerY + radius * sinf(base + 2.2f);
      drawGfxTriangle(centerX, centerY, inner, x0, y0, left, x1, y1, right);
   }
}

// the noise texture drawn at a quarter of its own size, so each pixel reads a
// different part of it. this is the load the triangles cannot make: texture
// fetch, filtering, and the video memory behind them.
static void drawNoiseLayers(int layers)
{
   if (layers == 0 || noiseTexture.offset == 0) return;

   int screenWidth = getGfxScreenWidth(), screenHeight = getGfxScreenHeight();
   int layerWidth = screenWidth / 2, layerHeight = screenHeight / 2;
   float driftX = screenWidth / 5.0f, driftY = screenHeight / 5.0f;   // keeps every layer fully on screen

   for (int layer = 0; layer < layers; layer++)
   {
      float angle = spin * 0.7f + layer * 6.2831853f / layers;
      int x = (int)(screenWidth / 2.0f + cosf(angle) * driftX) - layerWidth / 2;
      int y = (int)(screenHeight / 2.0f + sinf(angle) * driftY) - layerHeight / 2;
      drawGfxTexture(x, y, layerWidth, layerHeight, noiseTexture, 0, 0, 1, 1, NOISE_LAYER_TINT, GFX_FILTER_LINEAR);
   }
}

void drawStressCanvas(void)
{
   spin += SPIN_PER_FRAME;
   if (suspended) return;

   // triangles first: a texture change splits the batch, so drawing them mixed
   // together would cost a batch flush per shape
   drawTriangleFan(GPU_TRIANGLES[currentLoad.gpuLevel]);
   drawNoiseLayers(GPU_NOISE_LAYERS[currentLoad.gpuLevel]);
}
