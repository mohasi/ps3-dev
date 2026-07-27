#include "lookup.h"
#include "toc.h"
#include "cddb.h"
#include "amg-response.h"
#include "syscall.h"     // sysMemAllocate/Free, SYS_PAGE_64K
#include "string-utilities.h"  // appendStr (no NUL - caller adds it), appendUint64, memCopy
#include "dbg.h"

#include <cell/http.h>
#include <cell/http/util.h>

#define TAG "[cdi] "

#define GNUDB_HOST   "http://gnudb.gnudb.org"
#define GNUDB_HELLO  "&hello=user+ps3+abcde+2.9.3&proto=6"
#define HDR_RESERVE  128            // space kept at the front of out for the (right-sized) HTTP header

// one 64 KB work page carved into: arena scratch, the read-record buffer, the query-reply buffer.
#define WORK_SIZE     0x10000
#define SCRATCH_CAP   0xA000        // 40 KB  (30-track build measured at ~34 KB)
#define RBUF_CAP      0x4000        // 16 KB  (a gnudb record is ~2 KB, headroom for long titles)
#define QBUF_CAP      (WORK_SIZE - SCRATCH_CAP - RBUF_CAP)   // ~8 KB

// one plain-HTTP GET, reusing vsh's http stack (no init, no pools - the XMB already carved them). the
// body (up to cap) lands in out/*outLen; the HTTP status goes to *status. returns 0 when a response
// arrived (inspect *status), -1 on a connect/transport failure. one fresh client per call, drained
// then destroyed. gnudb is plain http, so no ssl callback is needed.
static int httpGet(const char *url, char *out, int cap, int *outLen, int *status)
{
   *outLen = 0;
   *status = -1;
   int result = -1;
   CellHttpClientId client = 0;
   CellHttpTransId  trans = 0;
   if (cellHttpCreateClient(&client) < 0 || !client) return -1;
   cellHttpClientSetConnTimeout(client, 10 * 1000 * 1000);
   cellHttpClientSetRecvTimeout(client, 10 * 1000 * 1000);

   size_t poolSize = 0;
   if (cellHttpUtilParseUri(NULL, url, NULL, 0, &poolSize) < 0) goto cleanup;
   char uriPool[1024];
   if (poolSize > sizeof uriPool) goto cleanup;
   CellHttpUri uri;
   if (cellHttpUtilParseUri(&uri, url, uriPool, poolSize, NULL) < 0) goto cleanup;
   if (cellHttpCreateTransaction(&trans, client, CELL_HTTP_METHOD_GET, &uri) < 0) { trans = 0; goto cleanup; }

   CellHttpHeader agent = { "User-Agent", "ps3" };
   cellHttpRequestAddHeader(trans, &agent);

   size_t sent = 0;
   if (cellHttpSendRequest(trans, NULL, 0, &sent) < 0) goto cleanup;   // connect + send
   int code = 0;
   if (cellHttpResponseGetStatusCode(trans, &code) < 0) goto cleanup;
   *status = code;

   int total = 0, truncated = 0;
   for (;;) {
      if (total >= cap - 1) { truncated = 1; break; }   // reply bigger than the buffer
      size_t got = 0;
      int rc = cellHttpRecvResponse(trans, out + total, (size_t)(cap - 1 - total), &got);
      if (rc < 0) { *status = -1; goto cleanup; }
      if (got == 0) break;   // clean end of body
      total += (int)got;
   }
   if (truncated) { logError(TAG "http: reply exceeds %d bytes, discarding\n", cap - 1); goto cleanup; }
   out[total] = '\0';   // gnudb replies are parsed as text
   *outLen = total;
   result = 0;

cleanup:
   if (trans)  cellHttpDestroyTransaction(trans);
   if (client) cellHttpDestroyClient(client);
   return result;
}

int buildLiveResponse(char *out, int outCap)
{
   static uint32_t offsets[99];
   static AmgTrack trackBuf[99];
   uint32_t leadout = 0, discId = 0, workAddr = 0;
   int tracks, result = -1, at, queryLen = 0, queryStatus = 0, recordLen = 0, recordStatus = 0, titleCount, bodyLen, headerLen;
   unsigned char *scratch;
   char *recordBuffer, *queryBuffer;
   char url[1400], category[24], id[16], header[HDR_RESERVE];
   AmgAlbum album;

   // 1) disc TOC -> CDDB disc id
   tracks = readCdToc(offsets, 99, &leadout);
   if (tracks < 1 || leadout == 0) return -1;
   discId = computeCddbDiscId(offsets, tracks, leadout);

   // 2) one on-demand 64 KB work page: arena scratch + record buffer + query buffer
   if (sysMemAllocate(WORK_SIZE, SYS_PAGE_64K, &workAddr) < 0 || !workAddr) { logError(TAG "live: no work page\n"); return -1; }
   scratch      = (unsigned char *)(uintptr_t)workAddr;
   recordBuffer = (char *)(uintptr_t)(workAddr + SCRATCH_CAP);
   queryBuffer  = (char *)(uintptr_t)(workAddr + SCRATCH_CAP + RBUF_CAP);

   // 3) gnudb QUERY: pick the matching record's category + id
   at = 0; appendStr(url, sizeof url, &at, GNUDB_HOST);
   if (buildCddbQueryUrl(url + at, (int)sizeof url - at, discId, offsets, tracks, leadout) < 0) goto done;
   if (httpGet(url, queryBuffer, QBUF_CAP, &queryLen, &queryStatus) != 0 || queryStatus != 200) { logError(TAG "live: query http status=%d\n", queryStatus); goto done; }
   if (parseCddbQuery(queryBuffer, category, sizeof category, id, sizeof id) != 0) { logInfo(TAG "live: no gnudb match for %08x\n", (unsigned)discId); goto done; }

   // 4) gnudb READ: fetch the full record (appendStr does not terminate; add the NUL for httpGet)
   at = 0;
   appendStr(url, sizeof url, &at, GNUDB_HOST);
   appendStr(url, sizeof url, &at, "/~cddb/cddb.cgi?cmd=cddb+read+");
   appendStr(url, sizeof url, &at, category);
   appendStr(url, sizeof url, &at, "+");
   appendStr(url, sizeof url, &at, id);
   appendStr(url, sizeof url, &at, GNUDB_HELLO);
   url[at] = '\0';
   if (httpGet(url, recordBuffer, RBUF_CAP, &recordLen, &recordStatus) != 0 || recordStatus != 200) { logError(TAG "live: read http status=%d\n", recordStatus); goto done; }

   // 5) parse -> album -> AMG body (built into out, past the reserved header room). Never emit more
   // tracks than the disc physically has, so a padded/edited gnudb record can't over-fill the album.
   titleCount = parseCddbRecord(recordBuffer, &album, trackBuf, 99);
   if (titleCount < 1) { logInfo(TAG "live: empty record\n"); goto done; }
   if (titleCount != tracks) logWarn(TAG "live: record has %d titles, disc has %d tracks\n", titleCount, tracks);
   if (album.trackCount > tracks) album.trackCount = tracks;
   bodyLen = buildAmgResponse(&album, scratch, SCRATCH_CAP, (unsigned char *)out + HDR_RESERVE, outCap - HDR_RESERVE);
   if (bodyLen < 0) { logError(TAG "live: build overflow\n"); goto done; }

   // 6) HTTP header (same shape the static file uses), then slide the body up against it
   headerLen = 0;
   appendStr(header, sizeof header, &headerLen, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: ");
   headerLen = appendUint64(header, sizeof header, headerLen, (uint64_t)bodyLen);
   appendStr(header, sizeof header, &headerLen, "\r\n\r\n");
   memCopy(out, header, headerLen);
   // slide the body down against the header (dst < src, and memCopy copies front-to-back, so the overlap is safe)
   memCopy(out + headerLen, out + HDR_RESERVE, bodyLen);
   result = headerLen + bodyLen;
   logInfo(TAG "live: %s/%s -> %d-byte response (%d tracks)\n", category, id, result, album.trackCount);

done:
   sysMemFree(workAddr);
   return result;
}
