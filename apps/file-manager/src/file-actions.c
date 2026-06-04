#include "file-actions.h"
#include "widgets/file-list.h"
#include "overlays/confirm-overlay.h"
#include "osk-input.h"
#include "dbg.h"
#include <stdio.h>

static void onDeleteConfirmed(bool confirmed)
{
    if (confirmed) deleteSelection();
}

// keyboard closed: text is the confirmed name (already stripped of illegal
// characters by the OSK filter), or NULL on cancel/empty. the active row is
// unchanged while the keyboard is up, so renameActiveEntry still targets it.
static void onRenameDone(const char *text)
{
    if (text) renameActiveEntry(text);
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

        case ACTION_RENAME: {
            // rename targets the highlighted row only, ignoring checkboxes. show the
            // keyboard pre-filled with the current name; it pumps on the main loop's
            // sysutil callback and the result comes back through onRenameDone.
            const char *name = getActiveEntryName();
            if (name) oskInputBegin("Rename", name, onRenameDone);
            break;
        }

        default:
            logInfo("[file-actions] %s: not implemented yet\n", getActionTitle(action));
            break;
    }
}
