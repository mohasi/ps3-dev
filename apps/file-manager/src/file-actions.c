#include "file-actions.h"
#include "widgets/file-list.h"
#include "overlays/confirm-overlay.h"
#include "osk-input.h"
#include "dbg.h"
#include "path.h"
#include "string-utilities.h"
#include <stdio.h>

// default field text the keyboard opens with for each kind of create.
#define NEW_FILE_DEFAULT    "New File.txt"
#define NEW_FOLDER_DEFAULT  "New Folder"

static void onDeleteConfirmed(ConfirmChoice choice)
{
   if (choice == CONFIRM_CROSS) deleteSelection();
}

// keyboard results. text is the confirmed name, or NULL on cancel/empty. each
// just hands off to file-list, which owns validation, collision handling and any
// follow-up prompt (overwrite / merge). the active row is unchanged while the
// keyboard is up, so renameActiveTo still targets it.
static void onRenameDone(const char *text)    { if (text) renameActiveTo(text); }
static void onNewFileDone(const char *text)    { if (text) createFile(text); }
static void onNewFolderDone(const char *text)  { if (text) createFolder(text); }
static void onZipNameDone(const char *text)    { if (text) zipSelectionTo(text); }

void dispatchAction(SelectionAction action)
{
   switch (action) {
      case ACTION_CUT:
         cutSelection();
         break;

      case ACTION_COPY:
         copySelection();
         break;

      case ACTION_PASTE:
         pasteClipboard();
         break;

      case ACTION_DELETE: {
         int n = getSelectionCount();
         char message[64];
         snprintf(message, sizeof message, "Are you sure you want to delete %d %s?", n, n == 1 ? "item" : "items");
         askConfirm("Delete Confirmation", message, "Yes", NULL, "No", onDeleteConfirmed);
         break;
      }

      case ACTION_RENAME: {
         // rename targets the highlighted row only; pre-fill the keyboard with
         // its current name and let file-list resolve the result.
         const char *name = getActiveEntryName();
         if (name) oskInputBegin("Rename", name, onRenameDone);
         break;
      }

      case ACTION_NEW_FILE:
         oskInputBegin("New File", NEW_FILE_DEFAULT, onNewFileDone);
         break;

      case ACTION_NEW_FOLDER:
         oskInputBegin("New Folder", NEW_FOLDER_DEFAULT, onNewFolderDone);
         break;

      case ACTION_ZIP: {
         // default text: the active entry's name with its extension swapped for
         // .zip (a folder's name has none, so it's just appended).
         const char *name = getActiveEntryName();
         if (!name) break;

         char defaultName[MAX_PATH_LEN];
         strCopy(defaultName, sizeof defaultName, name);
         const char *ext = getExtension(defaultName);
         if (ext) defaultName[ext - 1 - defaultName] = '\0';   // cut at the '.'
         int offset = getStrLen(defaultName);
         appendStr(defaultName, sizeof defaultName, &offset, ".zip");
         defaultName[offset] = '\0';   // appendStr does not terminate - leaves trailing stack garbage otherwise
         oskInputBegin("Zip", defaultName, onZipNameDone);
         break;
      }

      case ACTION_UNZIP:
         unzipActive();
         break;

      default:
         logInfo("[file-actions] %s: not implemented yet\n", getActionTitle(action));
         break;
   }
}
