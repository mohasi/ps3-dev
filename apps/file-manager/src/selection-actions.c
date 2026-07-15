#include "selection-actions.h"

typedef struct {
   const char *title;
   const char *subtitle;
   IconId      icon;
} ActionPresentation;

// note: PROPERTIES borrows the gear glyph (no info glyph in the set yet); NEW_FILE/NEW_FOLDER show a
// plain file/folder for now - a file+plus / folder+plus composite is the next step. Add glyphs in
// Fontello and update here if wanted.
static const ActionPresentation table[ACTION_COUNT] = {
   [ACTION_COPY]       = { "Copy",       "Copy selected file(s) to clipboard.",   ICON_DOCS         },
   [ACTION_CUT]        = { "Cut",        "Cut selected file(s) to clipboard.",    ICON_SCISSORS     },
   [ACTION_PASTE]      = { "Paste",      "Paste files from clipboard here.",      ICON_PASTE        },
   [ACTION_DELETE]     = { "Delete",     "Permanently delete selected file(s).",  ICON_TRASH_EMPTY  },
   [ACTION_RENAME]     = { "Rename",     "Rename the selected file or folder.",   ICON_EDIT         },
   [ACTION_NEW_FILE]   = { "New File",   "Create a new file here.",               ICON_DOC          },
   [ACTION_NEW_FOLDER] = { "New Folder", "Create a new folder here.",             ICON_FOLDER_EMPTY },
   [ACTION_EDIT]       = { "Edit",       "Open the selected file in an editor.",  ICON_DOC_TEXT     },
   [ACTION_ZIP]        = { "Zip",        "Compress selected file(s) to archive.", ICON_FILE_ARCHIVE },
   [ACTION_UNZIP]      = { "Unzip",      "Extract the selected archive.",         ICON_BOX          },
   [ACTION_MOUNT]      = { "Mount",      "Mount the selected disc image.",        ICON_CD           },
   [ACTION_PROPERTIES] = { "Properties", "Show details about the selection.",     ICON_COG          },
};

const char *getActionTitle(SelectionAction action)    { return table[action].title; }
const char *getActionSubtitle(SelectionAction action) { return table[action].subtitle; }
IconId      getActionIcon(SelectionAction action)     { return table[action].icon; }
