// spuburn - the program each stressed SPU runs: a tight vector multiply-add
// loop that never finishes. the ppu side stops these by terminating the whole
// thread group, so there is no shutdown handshake to get wrong here.
//
// built by the separate SPU compiler and embedded in the app's binary; see the
// BuildSpuBurn target in thermal-bench.vcxproj.
#include <spu_intrinsics.h>

int main(unsigned long long spuNumber, unsigned long long unused)
{
   (void)unused;

   // seed per spu so every unit works on different data
   vector float accumulator = spu_splats((float)spuNumber + 1.0f);
   vector float scale       = spu_splats(1.0000001f);
   vector float offset      = spu_splats(0.0000001f);

   // four independent chains keep the pipeline full instead of stalling on the
   // previous result - that is the difference between warm and hot
   vector float a = accumulator, b = accumulator, c = accumulator, d = accumulator;

   // volatile sink: without a visible result the compiler is free to delete the
   // whole loop body
   static volatile vector float sink;

   while (1)
   {
      a = spu_madd(a, scale, offset);
      b = spu_madd(b, scale, offset);
      c = spu_madd(c, scale, offset);
      d = spu_madd(d, scale, offset);
      sink = spu_add(spu_add(a, b), spu_add(c, d));
   }

   return 0;
}
