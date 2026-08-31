// spuburn - the program each stressed SPU runs. it never finishes; the ppu side
// stops these by terminating the whole thread group, so there is no shutdown
// handshake to get wrong here.
//
// two things run at once because either alone leaves half the unit idle: six
// multiply-add chains on the even pipe and six shuffle chains on the odd pipe,
// interleaved so both pipes have work every cycle. six chains each because a
// result is six cycles behind its input, and fewer chains would wait on
// themselves. shape taken from cellmark's spu_dual_kernel, which measures this
// as the heaviest load an spu can be put under.
//
// a ring of memory transfers runs alongside the arithmetic. the memory
// interface sits on the same chip and draws its own power, so a compute-only
// burn leaves a real part of the die cold.
//
// built by the separate SPU compiler and embedded in the app's binary; see the
// BuildSpuBurn target in thermal-bench.vcxproj.
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

#define CHAIN_COUNT   6
#define BURST_LENGTH  512        // chain steps between transfer checks, about a microsecond

#define RING_SLOTS    4
#define CHUNK_BYTES   16384
#define FIRST_TAG     1
#define RING_TAG_MASK (((1u << RING_SLOTS) - 1u) << FIRST_TAG)

static unsigned char transferWindow[RING_SLOTS][CHUNK_BYTES] __attribute__((aligned(128)));

static unsigned long long trafficBase;
static unsigned int trafficWindowBytes;
static unsigned int trafficOffset;

// half the slots read and half write, so both directions of the path are busy
static void issueTransfer(unsigned int slot)
{
   unsigned long long address = trafficBase + trafficOffset;
   if (slot < RING_SLOTS / 2) mfc_get(transferWindow[slot], address, CHUNK_BYTES, FIRST_TAG + slot, 0, 0);
   else                       mfc_put(transferWindow[slot], address, CHUNK_BYTES, FIRST_TAG + slot, 0, 0);

   trafficOffset += CHUNK_BYTES;
   if (trafficOffset + CHUNK_BYTES > trafficWindowBytes) trafficOffset = 0;
}

// without a visible result the compiler is free to delete the whole loop body
static volatile vector float floatSink;
static volatile vector unsigned char byteSink;

int main(unsigned long long spuIndex, unsigned long long bufferAddress, unsigned long long bufferBytes,
         unsigned long long unused)
{
   (void)unused;

   vector float scale  = spu_splats(0.99999988f);
   vector float offset = spu_splats(1.1920929e-7f);
   vector unsigned char rotateBytes = (vector unsigned char){ 1,2,3,0, 5,6,7,4, 9,10,11,8, 13,14,15,12 };

   // the chains are written out rather than looped over: a loop index would keep
   // them in local store instead of registers, which is the whole point of them
   vector float multiplyChain[CHAIN_COUNT];
   vector unsigned char shuffleChain[CHAIN_COUNT];
#define EACH_CHAIN(action) action(0) action(1) action(2) action(3) action(4) action(5)
#define SEED_CHAIN(index)  multiplyChain[index] = spu_splats((float)(spuIndex + (index) + 1) * 0.1f); \
                           shuffleChain[index]  = spu_splats((unsigned char)(spuIndex + (index) + 1));
#define STEP_CHAIN(index)  multiplyChain[index] = spu_madd(multiplyChain[index], scale, offset); \
                           shuffleChain[index]  = spu_shuffle(shuffleChain[index], shuffleChain[index], rotateBytes);

   EACH_CHAIN(SEED_CHAIN)

   trafficBase        = bufferAddress;
   trafficWindowBytes = (unsigned int)bufferBytes;
   int transfersRunning = bufferAddress != 0 && trafficWindowBytes >= RING_SLOTS * CHUNK_BYTES;
   if (transfersRunning)
      for (unsigned int slot = 0; slot < RING_SLOTS; slot++) issueTransfer(slot);

   for (;;)
   {
      for (int step = 0; step < BURST_LENGTH; step++) { EACH_CHAIN(STEP_CHAIN) }

      floatSink = spu_add(spu_add(multiplyChain[0], multiplyChain[1]), spu_add(multiplyChain[2], multiplyChain[3]));
      byteSink  = spu_or(shuffleChain[4], shuffleChain[5]);

      if (!transfersRunning) continue;

      // reissue whatever finished; asking without blocking keeps the pipes fed
      mfc_write_tag_mask(RING_TAG_MASK);
      mfc_write_tag_update(MFC_TAG_UPDATE_IMMEDIATE);
      unsigned int finished = mfc_read_tag_status();
      for (unsigned int slot = 0; slot < RING_SLOTS; slot++)
         if (finished & (1u << (FIRST_TAG + slot))) issueTransfer(slot);
   }

   return 0;
}
