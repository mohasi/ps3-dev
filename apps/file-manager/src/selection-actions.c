#include "selection-actions.h"
#include "sprite-regions.h"

typedef struct {
    const char *title;
    const char *subtitle;
    int         spriteId;
} ActionPresentation;

static const ActionPresentation table[ACTION_COUNT] = {
    [ACTION_COPY]       = { "Copy",       "Copy selected file(s) to clipboard.",   SPRITE_COPY       },
    [ACTION_CUT]        = { "Cut",        "Cut selected file(s) to clipboard.",    SPRITE_CUT        },
    [ACTION_PASTE]      = { "Paste",      "Paste files from clipboard here.",      SPRITE_PASTE      },
    [ACTION_DELETE]     = { "Delete",     "Permanently delete selected file(s).",  SPRITE_DELETE     },
    [ACTION_RENAME]     = { "Rename",     "Rename the selected file or folder.",   SPRITE_RENAME     },
    [ACTION_NEW_FILE]   = { "New File",   "Create a new file here.",               SPRITE_NEW_FILE   },
    [ACTION_NEW_FOLDER] = { "New Folder", "Create a new folder here.",             SPRITE_NEW_FOLDER },
    [ACTION_EDIT]       = { "Edit",       "Open the selected file in an editor.",  SPRITE_EDIT       },
    [ACTION_ZIP]        = { "Zip",        "Compress selected file(s) to archive.", SPRITE_COMPRESSED },
    [ACTION_UNZIP]      = { "Unzip",      "Extract the selected archive.",         SPRITE_PACKAGE    },
    [ACTION_MOUNT]      = { "Mount",      "Mount the selected disc image.",        SPRITE_DISC_ISO   },
    [ACTION_PROPERTIES] = { "Properties", "Show details about the selection.",     SPRITE_INFO       },
};

const char  *getActionTitle(SelectionAction action)    { return table[action].title; }
const char  *getActionSubtitle(SelectionAction action) { return table[action].subtitle; }
SpriteRegion getActionIcon(SelectionAction action)     { return spriteRegions[table[action].spriteId]; }
