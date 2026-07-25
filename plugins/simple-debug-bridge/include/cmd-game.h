#pragma once

// starting and quitting titles.
//
//   launch <TITLE_ID>   point the dashboard's /app_home entry at an installed
//                       title and press its icon - the same route every cfw
//                       tool uses, with the xmb doing the actual boot
//   exit-game           quit whatever is running, back to the xmb
//
// both go through the xmb's own plugins, reached as paf "views": explore_plugin
// (the dashboard itself) and game_plugin (only present while a title runs).
// each view hands out a table of function pointers at fixed slots
// (psdevwiki: Explore_plugin / Game_plugin), so we index it rather than
// declaring the hundred-odd entries we don't use.

#include "dbg.h"
#include "string-utilities.h"
#include "thread.h"
#include "cmd-common.h"
#include "pkg.h"      // isValidTitleId
#include "fileio.h"   // FILE_PATH_MAX
#include "vfs.h"      // isDir
#include "syscall.h"  // scCall4

extern uint32_t  paf_F21655F3(const char *pluginName);              // paf::View::Find
extern uint32_t *paf_23AFB290(uint32_t view, uint32_t identifier);  // paf::View::GetInterface

enum {
   EXPLORE_SLOT_EXEC_COMMAND = 6,     // explore_plugin: run an xmb command
   GAME_SLOT_EXIT            = 3,     // game_plugin: ExitGame(mode)
   GAME_EXIT_TO_XMB          = 0,
   SYSCALL_COBRA             = 8,     // cfw multi-purpose syscall
   COBRA_OPCODE_MAP_PATHS    = 0x7964 // remap one system path onto another
};

// a plausible mapped address - guards against calling through a view that
// answered but handed back nothing usable.
static int isReadableAddress(uint32_t address)
{
   return address >= 0x00010000 && address <= 0x3FFFFFFF;
}

static uint32_t *getViewInterface(const char *pluginName)
{
   uint32_t view = paf_F21655F3(pluginName);
   if (view == 0) return 0;
   uint32_t *table = paf_23AFB290(view, 1);
   if (!isReadableAddress((uint32_t)(uintptr_t)table)) return 0;
   return table;
}

// the dashboard drives itself with short text commands - the same ones it
// sends internally when the cursor moves or X is pressed.
typedef int (*ExploreCommandFn)(const char *command, void *callback, int unknown);

static void runXmbCommand(const char *command, unsigned settleMs)
{
   uint32_t *table = getViewInterface("explore_plugin");
   if (!table) return;
   ((ExploreCommandFn)(uintptr_t)table[EXPLORE_SLOT_EXEC_COMMAND])(command, 0, 0);
   sleepMs(settleMs);
}

// cobra's path remapper: makes one path resolve to another system-wide.
static int32_t mapSystemPath(const char *fromPath, const char *toPath)
{
   const char *fromList[1] = { fromPath };
   const char *toList[1]   = { toPath };
   return (int32_t)scCall4(SYSCALL_COBRA, COBRA_OPCODE_MAP_PATHS,
                           (uint64_t)(uintptr_t)fromList, (uint64_t)(uintptr_t)toList, 1);
}

static void cmdLaunchTitle(int cli, const char *args)
{
   char reply[192];

   if (!isValidTitleId(args)) {
      sendReply(cli, SDB_ERR, "usage: launch <TITLE_ID>");
      return;
   }

   // point /app_home at the title
   char gamePath[FILE_PATH_MAX];
   snprintf(gamePath, sizeof gamePath, "/dev_hdd0/game/%s", args);
   if (!isDir(gamePath)) {
      snprintf(reply, sizeof reply, "%s is not installed", args);
      sendReply(cli, SDB_ERR, reply);
      return;
   }
   int32_t mapRc = mapSystemPath("/app_home/PS3_GAME", gamePath);
   logInfo("[sdb] launch %s: map /app_home -> %s rc=0x%x\n", args, gamePath, mapRc);
   if (mapRc != 0) {
      snprintf(reply, sizeof reply, "could not map /app_home, rc=0x%x", mapRc);
      sendReply(cli, SDB_ERR, reply);
      return;
   }

   // walk the dashboard onto that icon and press it
   runXmbCommand("close_all_list", 200);
   runXmbCommand("focus_category game", 150);
   runXmbCommand("focus_segment_index seg_gamedebug", 250);
   runXmbCommand("exec_push", 0);
   logInfo("[sdb] launch %s: exec_push sent\n", args);

   snprintf(reply, sizeof reply, "launching %s", args);
   sendReply(cli, SDB_OK, reply);
}

static void cmdExitGame(int cli, const char *args)
{
   (void)args;

   // game_plugin exists only while a title is running, so its absence is the
   // "nothing to quit" answer.
   uint32_t *table = getViewInterface("game_plugin");
   if (!table) {
      sendReply(cli, SDB_ERR, "nothing running");
      return;
   }

   sendReply(cli, SDB_OK, "exiting to xmb");
   typedef int32_t (*GameExitFn)(int32_t mode);
   logInfo("[sdb] exit-game via 0x%08x\n", table[GAME_SLOT_EXIT]);
   int32_t rc = ((GameExitFn)(uintptr_t)table[GAME_SLOT_EXIT])(GAME_EXIT_TO_XMB);
   logInfo("[sdb] exit-game returned rc=0x%x\n", rc);
}
