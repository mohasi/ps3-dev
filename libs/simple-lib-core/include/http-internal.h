#pragma once

// http-internal - glue shared by http.c (light: transport binding + one-shot request/response) and
// http-stream.c (heavy: the ring-buffer streaming engine). Kept out of the public http.h so the streaming
// engine can sit in its own translation unit: a malloc-free PRX that only calls isHttpUrl/fetchHttp pulls
// http.c but NOT http-stream.c, so its malloc + 4 MB ring + prefetch thread never reach the plugin.

#include "http-transport.h"   // HttpTransport, HttpHeader

// a widely-accepted UA; some CDNs 403 a request with none. generic on purpose - not host-specific.
#define DEFAULT_UA "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36"

// the transport bound by initSystemHttp/initModernHttp, or NULL. http-stream.c reads it through this
// accessor so the light http.c never references (and never pulls in) the streaming TU.
const HttpTransport *getActiveTransport(void);
