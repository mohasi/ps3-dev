#include "sound.h"

#include <string.h>
#include <stdlib.h>

#include "audio.h"     // lib mixer (loadSfxMem / playSfx / ...)
#include "rpk.h"
#include "config.h"
#include "gamepath.h"  // getGameRpkPath()
#include "printf.h"
#include "dbg.h"

#define SFX_POOL 6     // one-shot slots; the mixer caps total streams at 8 (music takes 1)

// Ren'Py's default mixer volume is 1.0 for every channel (engine audio.py), and this game
// overrides none -- so 1.0 matches the PC build's level.
#define MUSIC_VOLUME 1.0f
#define SFX_VOLUME   1.0f

static int         ready;
static Audio       music;
static int         musicOn;
static const char *curMusicCmd;     // stable token of the current `play music` command (NULL = none)
static Audio       sfx[SFX_POOL];
static int         sfxCursor;

void initSound(void)
{
   musicOn = 0;
   curMusicCmd = NULL;
   sfxCursor = 0;
   memset(&music, 0, sizeof music);
   memset(sfx, 0, sizeof sfx);
   ready = (initSfx() == 0);
   if (!ready) logWarn("[rpp] audio init failed; running silent\n");
}

// basename: the part after the last '/'.
static const char *baseName(const char *path)
{
   const char *slash = strrchr(path, '/');
   return slash ? slash + 1 : path;
}

// Script audio name -> rpk asset suffix "/<base>.ogg". The converter re-encodes every
// sound to ogg, keeping the basename ("swish.wav" -> swish.ogg, "music/x.ogg" -> x.ogg);
// matching by suffix lets the bundle keep the original subdirectories.
static void toOggSuffix(const char *name, char *out, int cap)
{
   const char *base = baseName(name);
   int len = 0;
   out[len++] = '/';
   for (const char *ch = base; *ch && *ch != '.' && len < cap - 5; ch++) out[len++] = *ch;
   const char *ext = ".ogg";
   for (int i = 0; ext[i] && len < cap - 1; i++) out[len++] = ext[i];
   out[len] = '\0';
}

// Full-path variant: keep the script's whole relative path ("bgm/x.mp3" -> "/bgm/x.ogg"), ext -> ogg.
// Trying this FIRST disambiguates two assets that share a basename in different folders (the converter
// stores full relative paths); the basename suffix remains a fallback so nothing regresses.
static void toFullOggSuffix(const char *name, char *out, int cap)
{
   while (*name == '/' ) name++;
   int dot = -1; for (int i = 0; name[i]; i++) if (name[i] == '.') dot = i;
   int len = 0; out[len++] = '/';
   for (int i = 0; name[i] && (dot < 0 || i < dot) && len < cap - 5; i++) out[len++] = name[i];
   const char *ext = ".ogg";
   for (int i = 0; ext[i] && len < cap - 1; i++) out[len++] = ext[i];
   out[len] = '\0';
}

// Reads an audio asset from the rpk into a malloc'd buffer (caller frees). 0 on failure.
static int readAudioAsset(const char *scriptName, unsigned char **buf, long *len)
{
   RpkFile rpk;
   if (openRpk(&rpk, getGameRpkPath()) != 0) return 0;
   char entryName[256];
   char full[256]; toFullOggSuffix(scriptName, full, sizeof full);
   int readResult = readRpkEntrySuffix(&rpk, full, 0, entryName, sizeof entryName, buf, len);
   if (readResult != 0 || !*buf)   // fall back to a basename-only match (path-less script names)
   {
      char suffix[96]; toOggSuffix(scriptName, suffix, sizeof suffix);
      readResult = readRpkEntrySuffix(&rpk, suffix, 0, entryName, sizeof entryName, buf, len);
   }
   closeRpk(&rpk);
   if (readResult != 0 || !*buf) { logWarn("[rpp] audio '%s' not in bundle\n", scriptName); return 0; }
   return 1;
}

static void playMusic(const char *file, float fadeIn)
{
   if (!ready) return;
   unsigned char *buf = NULL; long len = 0;
   if (!readAudioAsset(file, &buf, &len)) return;
   if (musicOn) { stopSfx(&music); freeSfx(&music); musicOn = 0; }   // replace current track
   music = loadSfxMem(buf, (uint32_t)len, SFX_STREAM);
   free(buf);
   if (music.vorbis || music.pcmData) {
      if (fadeIn > 0.0f) { playSfx(&music, 0.0f, 1.0f, 1 /*loop*/); fadeSfx(&music, MUSIC_VOLUME, fadeIn); }
      else                 playSfx(&music, MUSIC_VOLUME, 1.0f, 1 /*loop*/);
      musicOn = 1;
   }
   else logWarn("[rpp] music '%s' decode failed\n", file);
}

static void stopMusic(float fadeOut)
{
   if (!musicOn) return;
   if (fadeOut > 0.0f) fadeSfx(&music, 0.0f, fadeOut);   // mixer ramps to silence then stops;
   else { stopSfx(&music); freeSfx(&music); musicOn = 0; } // the handle is reclaimed on the next
                                             // play music / termSound.
}

static void playSound(const char *file)
{
   if (!ready) return;
   unsigned char *buf = NULL; long len = 0;
   if (!readAudioAsset(file, &buf, &len)) return;
   Audio *slot = &sfx[sfxCursor];                 // round-robin one-shot slots
   sfxCursor = (sfxCursor + 1) % SFX_POOL;
   if (slot->pcmData || slot->vorbis) { stopSfx(slot); freeSfx(slot); }
   *slot = loadSfxMem(buf, (uint32_t)len, SFX_MEMORY);
   free(buf);
   if (slot->pcmData) playSfx(slot, SFX_VOLUME, 1.0f, 0 /*no loop*/);
}

// Reads the float after a keyword (e.g. "fadein 1.0" -> 1.0); 0 if the keyword is absent.
static float fadeArg(const char *text, const char *keyword)
{
   const char *found = strstr(text, keyword);
   if (!found) return 0.0f;
   found += strlen(keyword);
   while (*found == ' ') found++;
   return (float)atof(found);
}

// Extracts the first double-quoted substring into out. Returns 1 on success.
static int firstQuoted(const char *text, char *out, int cap)
{
   const char *cur = strchr(text, '"');
   if (!cur) return 0;
   cur++;
   int len = 0;
   while (*cur && *cur != '"' && len < cap - 1) out[len++] = *cur++;
   out[len] = '\0';
   return len > 0;
}

void execSound(const char *cmd)
{
   if (!cmd) return;
   const char *cur = cmd;          // keep `cmd` as the stable rollback token
   while (*cur == ' ') cur++;
   char file[128];
   if (strncmp(cur, "play ", 5) == 0)
   {
      const char *rest = cur + 5;
      while (*rest == ' ') rest++;
      if      (strncmp(rest, "music", 5) == 0) { if (firstQuoted(rest, file, sizeof file)) { playMusic(file, fadeArg(rest, "fadein")); curMusicCmd = cmd; } }
      else if (strncmp(rest, "sound", 5) == 0) { if (firstQuoted(rest, file, sizeof file)) playSound(file); }
   }
   else if (strncmp(cur, "stop ", 5) == 0)
   {
      const char *rest = cur + 5;
      while (*rest == ' ') rest++;
      if (strncmp(rest, "music", 5) == 0) { stopMusic(fadeArg(rest, "fadeout")); curMusicCmd = NULL; }
   }
}

const char *getSoundMusicCmd(void) { return curMusicCmd; }

// Reapplies a music command instantly (no fade) when rolling back/forward; a no-op when
// the channel is already in that state (same token, including both NULL).
void restoreSoundMusic(const char *cmd)
{
   if (cmd == curMusicCmd) return;
   if (!cmd) { stopMusic(0.0f); curMusicCmd = NULL; return; }

   const char *cur = cmd;  while (*cur == ' ') cur++;
   const char *rest = cur + 5; while (*rest == ' ') rest++;   // past "play "
   char file[128];
   if (strncmp(cur, "play ", 5) == 0 && strncmp(rest, "music", 5) == 0 && firstQuoted(rest, file, sizeof file))
   {
      playMusic(file, 0.0f);
      curMusicCmd = cmd;
   }
}

void termSound(void)
{
   if (!ready) return;
   stopMusic(0.0f);   // instant, and frees the music handle
   for (int i = 0; i < SFX_POOL; i++)
      if (sfx[i].pcmData || sfx[i].vorbis) { stopSfx(&sfx[i]); freeSfx(&sfx[i]); }
   termSfx();
   ready = 0;
}
