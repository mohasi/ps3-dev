#pragma once

// xcloud-catalog - the proper name and the box art for each game.
//
// The streaming service only knows games by an identifier: ALBIONONLINE, CALLOFDUTYHQ. The store
// is a separate service that knows what they are called and what they look like, and the two are
// tied together by the product id the streaming service hands over alongside each game.
//
// Nothing here is required for a game to run. If the store cannot be reached the list still works,
// it just reads in identifiers.

#define CATALOG_NAME_MAX  64
#define CATALOG_IMAGE_MAX 192

// Fills in names and art for the games already fetched, in place. Asks in batches rather than one
// request per game. Returns how many it named, or -1 if the store could not be reached at all.
int fetchXcloudCatalog(void);

// The name and box art for a game by its place in the list. The name falls back to the identifier
// when the store had nothing, and the art is empty when there is none.
const char *getXcloudTitleName(int index);
const char *getXcloudTitleArtUrl(int index);
