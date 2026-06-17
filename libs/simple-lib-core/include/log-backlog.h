#pragma once

// pre-connect log line ring buffer.
//
// log producers (and the bridge itself) start emitting lines well before
// the host TCP connection is up. without a buffer those early lines would
// be silently dropped and the operator would never see startup output in
// the host Logs tab. this ring holds the most recent N lines; when full,
// the oldest line is overwritten.
//
// callers own the ring instance (one per bridge endpoint) and drain it
// once the host connection is established. drain is order-preserving.

#include "dbg.h"   // BacklogLine, LOG_LINE_MAX

#define LOG_BACKLOG_MAX 64

typedef struct {
   BacklogLine lines[LOG_BACKLOG_MAX];
   int head;    // next write slot
   int count;   // valid entries (<= LOG_BACKLOG_MAX)
} LogBacklog;

// callback fired once per buffered line during drain. return <0 to stop
// the drain early (caller will reset the ring regardless).
typedef int (*OnBacklogLine)(const char *data, int len, void *user);

static inline void pushLogBacklog(LogBacklog *ring, const char *data, int len)
{
   int take = len < LOG_LINE_MAX ? len : LOG_LINE_MAX;
   BacklogLine *slot = &ring->lines[ring->head];
   slot->len = take;
   for (int i = 0; i < take; i++) slot->data[i] = data[i];
   ring->head = (ring->head + 1) % LOG_BACKLOG_MAX;
   if (ring->count < LOG_BACKLOG_MAX) ring->count++;
}

static inline void drainLogBacklog(LogBacklog *ring, OnBacklogLine onLine, void *user)
{
   if (ring->count == 0) return;
   int start = (ring->head - ring->count + LOG_BACKLOG_MAX) % LOG_BACKLOG_MAX;
   for (int i = 0; i < ring->count; i++) {
     BacklogLine *slot = &ring->lines[(start + i) % LOG_BACKLOG_MAX];
     if (onLine(slot->data, slot->len, user) < 0) break;
   }
   ring->count = 0;
   ring->head  = 0;
}
