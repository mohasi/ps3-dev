// cellhttp-stack - firmware http/ssl/https bringup shared by all cellHttp users (see cellhttp-stack.h).

#include "cellhttp-stack.h"
#include "dbg.h"

#include <stdlib.h>

// pools are process-wide (carved by cellHttpInit/cellSslInit), not per-client. every simultaneous HTTPS
// connection draws its buffers + TLS record/handshake state from here at once. Sony's single-HTTPS sample
// budgets 64 KB HTTP / 256 KB SSL for ONE connection. peak concurrency is the checked-out workers plus the
// keep-alive client pool, whose idle clients each keep a live TLS context warm for reuse (~14 connections
// at once); sized well past that so a burst can't corrupt a body or fault inside libhttp/libssl. this is
// an EBOOT with hundreds of MB free - RAM is cheap here and undersizing is what breaks.
#define HTTP_POOL_SIZE (2 * 1024 * 1024)   // ~24 connections' worth of HTTP buffers
#define SSL_POOL_SIZE  (5 * 1024 * 1024)   // ~20 connections' worth of TLS state

// brought up once and kept resident for the whole app run (never torn down between plays). Cycling
// init->end->init faults inside libssl when a stream was torn down mid-download, so the stack stays up.
static int   stackUp;
static void *httpPool, *sslPool;

int32_t verifyCellHttpsCert(uint32_t verifyErr, CellSslCert const cert[], int certNum, const char *hostname, CellHttpSslId id, void *arg)
{
   (void)cert; (void)certNum; (void)hostname; (void)id; (void)arg;
   return verifyErr;   // non-zero fails the handshake
}

static int loadSystemCerts(size_t *numOut, CellHttpsData **listOut)
{
   size_t size = 0;
   int ret = cellSslCertificateLoader(CELL_SSL_LOAD_CERT_ALL, NULL, 0, &size);
   if (ret < 0) return ret;
   char *buffer = malloc(size);
   if (!buffer) return -1;
   ret = cellSslCertificateLoader(CELL_SSL_LOAD_CERT_ALL, buffer, size, NULL);
   if (ret < 0) { free(buffer); return ret; }
   CellHttpsData *list = malloc(sizeof(CellHttpsData));
   if (!list) { free(buffer); return -1; }
   list[0].ptr = buffer;
   list[0].size = size;
   *listOut = list;
   *numOut = 1;
   return 0;
}

int ensureHttpStack(void)
{
   if (stackUp) return 0;

   int httpOk = 0, sslOk = 0, httpsOk = 0;
   CellHttpsData *caList = NULL;
   size_t numCa = 0;
   int ret;

   httpPool = malloc(HTTP_POOL_SIZE);
   if (!httpPool || cellHttpInit(httpPool, HTTP_POOL_SIZE) < 0) goto fail;
   httpOk = 1;
   sslPool = malloc(SSL_POOL_SIZE);
   if (!sslPool || cellSslInit(sslPool, SSL_POOL_SIZE) < 0) goto fail;
   sslOk = 1;

   if (loadSystemCerts(&numCa, &caList) < 0) goto fail;
   ret = cellHttpsInit(numCa, caList);
   free(caList[0].ptr); free(caList);
   if (ret < 0) goto fail;
   httpsOk = 1;

   stackUp = 1;
   return 0;

fail:
   if (httpsOk) cellHttpsEnd();
   if (sslOk)   cellSslEnd();
   if (httpOk)  cellHttpEnd();
   free(sslPool);  sslPool = NULL;
   free(httpPool); httpPool = NULL;
   return -1;
}

void termHttpStack(void)
{
   if (!stackUp) return;
   cellHttpsEnd();
   cellSslEnd();
   cellHttpEnd();
   free(sslPool);  sslPool = NULL;
   free(httpPool); httpPool = NULL;
   stackUp = 0;
}
