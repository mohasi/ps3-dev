#pragma once

// cheat-fetch - the online read path. on game launch the menu thread calls maybeFetchForGame;
// if the mode allows it, the title is a real game, and there is no local cheat file yet, it
// downloads compiled/<titleId>.txt from the repo on its OWN thread (blocking network must not
// stall the caller) and saves it to the cheats dir so it is local from then on. toasts report
// progress. reuses the firmware http/ssl stack the xmb already has up (see net reuse notes).

// MENU thread, once per game: fetch this title's cheats in the background if warranted (online
// mode + real game id + not already local). quick guards + a spawned worker; never blocks.
void maybeFetchForGame(const char *titleId);

// MENU thread, on Update (Triangle): re-download this title's cheats and overwrite the local file,
// even if one already exists. same background worker; never blocks. returns 1 if a fetch started, 0
// if declined (busy / offline / not a game title) — enter update mode only when it returns 1.
int updateCheatsForGame(const char *titleId);

// the outcome of an Update download, handed from the fetch thread to the menu thread.
typedef enum UpdateResult {
   UPDATE_NONE = 0,        // no update result waiting (or already consumed)
   UPDATE_SAVED = 1,       // new file downloaded and saved -> refresh the menu
   UPDATE_NOT_FOUND = 2,   // the repo has no cheats for this game (404)
   UPDATE_ERROR = 3        // transport or save failure
} UpdateResult;

// MENU thread: read the pending Update outcome once, then clear it (atomic).
UpdateResult consumeUpdateResult(void);

// which way a player voted on a cheat. WORKED carries the pre-write originals (working-val); FAILED
// is an empty file. see the online-sync design's evidence model.
typedef enum VoteEvent { VOTE_WORKED, VOTE_FAILED } VoteEvent;

// MENU thread: send one anonymous vote as a github PR (branch + file + PR; the workflow auto-merges).
// non-blocking (spawns a worker). returns 1 if a vote started, 2 if this console already uploaded this
// exact vote (deduped, nothing sent), 0 if declined (already sending / not contribute mode / bad
// inputs). body = working-val lines for WORKED (empty otherwise).
int submitCheatVote(const char *titleId, const char *version, unsigned int cheatHash, VoteEvent event, const char *body);

// MENU thread: read the pending vote outcome once, then clear it (atomic) — 0 none, 1 sent, 2 failed.
int consumeVoteResult(void);
