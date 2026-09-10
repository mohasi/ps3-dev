#pragma once

// title-select - choosing which game to stream.
//
// There is no dashboard to connect to: a session is always started for one named game, so one has
// to be picked before anything can happen.

#include "font.h"

// Shows the games the account can play and returns the index of the one chosen, or -1 if the
// player backed out. The list has to have been fetched already.
int runTitleSelectScreen(Font *font);
