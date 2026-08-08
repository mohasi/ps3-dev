#include "wg-random.h"

#include <sys/random_number.h>

#include "blake2s.h"
#include "dbg.h"
#include "string-utilities.h"

#define TAG "[wg] "

#define CHUNK_LENGTH 32
#define RAW_LENGTH   64   // twice what is handed out, so the hash never runs short of input

int getRandomBytes(uint8_t *out, int length)
{
   uint8_t raw[RAW_LENGTH];
   uint8_t chunk[CHUNK_LENGTH];

   while (length > 0) {
      int rc = sys_get_random_number(raw, sizeof raw);
      if (rc != 0) {
         logError(TAG "random: sys_get_random_number failed, rc=0x%x\n", rc);
         memSet(raw, 0, sizeof raw);
         return -1;
      }

      hashBlake2s(chunk, sizeof chunk, raw, sizeof raw);

      int take = length < CHUNK_LENGTH ? length : CHUNK_LENGTH;
      memCopy(out, chunk, take);
      out += take;
      length -= take;
   }

   memSet(raw, 0, sizeof raw);
   memSet(chunk, 0, sizeof chunk);
   return 0;
}
