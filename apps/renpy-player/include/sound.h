#pragma once

// Player audio: maps Ren'Py `play`/`stop` statements onto the lib mixer. One looping
// music stream plus a small pool of one-shot sound effects, all loaded from the rpk.
// (fadein/fadeout are not applied yet -- play/stop are instant.)

void initSound(void);
void execSound(const char *cmd);   // run a `play music/sound "..."` or `stop music` line
void termSound(void);

// Music rollback: the music channel is part of Ren'Py's rollback state (one-shot sfx are
// not). getSoundMusicCmd() returns a stable token for the current music command (NULL = none)
// to snapshot per history frame; restoreSoundMusic() reapplies it (instant, no fade) when
// stepping back/forward, so the track follows the scene you're on.
const char *getSoundMusicCmd(void);
void        restoreSoundMusic(const char *cmd);
