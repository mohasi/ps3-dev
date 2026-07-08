#pragma once

// search screen - a VideoGrid in one of two modes: search results (a query) or a channel's videos. Pushed
// on top of the home screen; Circle backs out (channel -> search results -> home). SELECT cycles the sort.

#include "screen.h"
#include "extractor.h"

extern Screen searchScreen;

// push the search screen showing a text-search for `query`.
void openSearchQuery(const char *query);

// push the search screen showing the given result's channel (its videos only).
void openChannelFor(const SearchResult *item);
