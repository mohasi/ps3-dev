// xcloud-catalog - see xcloud-catalog.h.
//
// The store answers with far more than is wanted here: descriptions, ratings, every image at every
// size. Rather than model that, each product in the answer is walked over in turn and only the two
// things needed are lifted out of it.

#include "xcloud-catalog.h"

#include "dbg.h"
#include "http.h"
#include "json.h"
#include "printf.h"
#include "string-utilities.h"
#include "xcloud-api.h"

#include <stdlib.h>

#define TAG "[cst] "

#define CATALOG_HOST "https://displaycatalog.mp.microsoft.com/v7.0/products"

// How many games one request asks about, and how much room its answer needs. Measured against the
// store: eight games come back in 124 kbytes, so this leaves four times the room. The full form of
// the answer is five times larger and 22 of 96 games silently lost their name and art to a buffer
// that overflowed at eight, so the margin is the point.
#define BATCH 8
#define BODY_MAX (512 * 1024)

// which picture to use. the first that exists wins: a tall cover if there is one, else the square
// box, else the logo.
static const char *IMAGE_CHOICES[] = { "Poster", "BoxArt", "Logo" };

static char names[TITLE_LIST_MAX][CATALOG_NAME_MAX];
static char artUrls[TITLE_LIST_MAX][CATALOG_IMAGE_MAX];

const char *getXcloudTitleName(int index)
{
   if (index < 0 || index >= getXcloudTitleCount()) return "";
   if (names[index][0]) return names[index];

   const XcloudTitle *title = getXcloudTitle(index);
   return title ? title->id : "";
}

const char *getXcloudTitleArtUrl(int index)
{
   if (index < 0 || index >= getXcloudTitleCount()) return "";
   return artUrls[index];
}

// the store gives image addresses without the https on the front, and at their full size. asking
// for a height keeps the download to something a console wants to hold.
static void takeImageUrl(const char *properties, int length, char *out)
{
   for (int choice = 0; choice < (int)(sizeof IMAGE_CHOICES / sizeof IMAGE_CHOICES[0]); choice++) {
      int at = 0;
      while (at < length) {
         int found = findBytes(properties + at, length - at, IMAGE_CHOICES[choice],
                               getStrLen(IMAGE_CHOICES[choice]));
         if (found < 0) break;
         at += found;

         // the address sits beside the purpose inside the same entry, either side of it
         int entryStart = at;
         while (entryStart > 0 && properties[entryStart] != '{') entryStart--;
         int entryEnd = at;
         while (entryEnd < length && properties[entryEnd] != '}') entryEnd++;

         char uri[CATALOG_IMAGE_MAX];
         if (getJsonText(properties + entryStart, entryEnd - entryStart, "Uri", uri, sizeof uri) == 0 && uri[0]) {
            snprintf(out, CATALOG_IMAGE_MAX, "%s%s?h=180", startsWith(uri, "//") ? "https:" : "", uri);
            return;
         }
         at = entryEnd;
      }
   }
}

// walks the products in one answer and files each against the game it belongs to
static int readAnswer(const char *body, int length, int firstTitle, int titlesAsked)
{
   int named = 0;
   int listStart = 0, listEnd = 0;
   if (findJsonArray(body, length, "Products", &listStart, &listEnd) != 0) return 0;

   int productStart = 0, productEnd = 0, offset = listStart;
   while ((offset = readJsonObject(body, listEnd, offset, &productStart, &productEnd)) != 0) {
      const char *product = body + productStart;
      int productLength = productEnd - productStart;

      char productId[XCLOUD_PRODUCT_ID_MAX];
      if (getJsonText(product, productLength, "ProductId", productId, sizeof productId) != 0) continue;

      // The answer does not come back in the order it was asked, so each product is matched by id.
      // Case is ignored because the two services disagree about it: the streaming list gives some
      // ids in lower case and the store always answers in upper, which cost nine games their art.
      int index = -1;
      for (int candidate = firstTitle; candidate < firstTitle + titlesAsked; candidate++) {
         const XcloudTitle *title = getXcloudTitle(candidate);
         if (title && strCmpICase(title->productId, productId) == 0) { index = candidate; break; }
      }
      if (index < 0) continue;

      int propertiesStart = 0, propertiesEnd = 0;
      if (findJsonArray(product, productLength, "LocalizedProperties", &propertiesStart, &propertiesEnd) != 0)
         continue;

      const char *properties = product + propertiesStart;
      int propertiesLength = propertiesEnd - propertiesStart;

      if (getJsonText(properties, propertiesLength, "ProductTitle", names[index], CATALOG_NAME_MAX) == 0)
         named++;
      takeImageUrl(properties, propertiesLength, artUrls[index]);
   }
   return named;
}

int fetchXcloudCatalog(void)
{
   int count = getXcloudTitleCount();
   if (count <= 0) return 0;

   memSet(names, 0, sizeof names);
   memSet(artUrls, 0, sizeof artUrls);

   char *body = malloc(BODY_MAX);
   if (!body) return -1;

   int named = 0, requests = 0;
   for (int first = 0; first < count; first += BATCH) {
      int asked = count - first < BATCH ? count - first : BATCH;

      char url[1024];
      int at = snprintf(url, sizeof url, "%s?bigIds=", CATALOG_HOST);

      // the comma goes between ids, so it follows what was written rather than the loop: a title
      // with no product id at the front of a batch would otherwise leave one leading the list
      int written = 0;
      for (int which = 0; which < asked; which++) {
         const XcloudTitle *title = getXcloudTitle(first + which);
         if (!title || !title->productId[0]) continue;
         at += snprintf(url + at, sizeof url - at, "%s%s", written++ ? "," : "", title->productId);
      }
      if (!written) continue;
      // StoreSDK carries the name and every picture, without the descriptions, videos and offers
      // that make the full form five times the size for nothing that is used here
      at += snprintf(url + at, sizeof url - at, "&market=%s&languages=%s&fieldsTemplate=StoreSDK", "GB", "en-GB");

      int length = 0, status = 0;
      if (fetchHttp("GET", url, 0, 0, 0, 0, body, BODY_MAX, &length, &status) != 0 || status != 200) {
         logWarn(TAG "catalog: the store refused a batch (%d)\n", status);
         continue;
      }
      // the fetch truncates to fit and cannot say that it did, so a full buffer is the only sign
      // that the tail of an answer was lost, and with it the games it described
      if (length >= BODY_MAX - 1) logWarn(TAG "catalog: an answer filled the buffer, games may be missing\n");

      requests++;
      named += readAnswer(body, length, first, asked);
   }

   free(body);

   int withArt = 0;
   for (int index = 0; index < count; index++) withArt += artUrls[index][0] != 0;
   logInfo(TAG "catalog: named %d of %d over %d requests, %d have art\n", named, count, requests, withArt);

   for (int index = 0; index < count; index++) {
      if (artUrls[index][0]) continue;
      const XcloudTitle *title = getXcloudTitle(index);
      logWarn(TAG "catalog: the store had nothing for %s (%s)\n", title ? title->id : "?",
              title ? title->productId : "?");
   }
   return named;
}
