#pragma once

// selection-actions - shared vocabulary for actions a user can perform on a selection.
// owns the enum and presentation lookups (title, subtitle, icon).
// owns SelectionSummary - the read-only description of "what is selected".

#include "ui/icon-ids.h"

typedef enum {
   ACTION_COPY,
   ACTION_CUT,
   ACTION_PASTE,
   ACTION_DELETE,
   ACTION_RENAME,
   ACTION_NEW_FILE,
   ACTION_NEW_FOLDER,
   ACTION_EDIT,
   ACTION_ZIP,
   ACTION_UNZIP,
   ACTION_MOUNT,
   ACTION_PROPERTIES,
   ACTION_COUNT
} SelectionAction;

typedef struct {
   const char  *title;     // "report.txt"   or "3 items"
   const char  *subtitle;  // "Text"         or "Mixed"
   const char  *detail;    // "1.2 KB"       or "4.7 MB total"
   IconId       icon;
} SelectionSummary;

const char *getActionTitle(SelectionAction action);
const char *getActionSubtitle(SelectionAction action);
IconId      getActionIcon(SelectionAction action);
