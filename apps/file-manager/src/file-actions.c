#include "file-actions.h"
#include "widgets/file-list.h"
#include "overlays/confirm-overlay.h"
#include "dbg.h"
#include <stdio.h>

static void onDeleteConfirmed(bool confirmed)
{
    if (confirmed) deleteSelection();
}

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
            askConfirm("Delete Confirmation", message, "Yes", "No", onDeleteConfirmed);
            break;
        }
        default:
            logInfo("[file-actions] %s: not implemented yet\n", getActionTitle(action));
            break;
    }
}
