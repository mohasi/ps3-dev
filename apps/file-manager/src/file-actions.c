#include "file-actions.h"
#include "dbg.h"

void dispatchAction(SelectionAction action)
{
    logInfo("[file-actions] dispatch: %s\n", getActionTitle(action));
}
