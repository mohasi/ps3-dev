#pragma once

// file-actions - executes a SelectionAction against the current file-list selection.
// owns clipboard state internally. may push confirm / progress overlays as needed.

#include "selection-actions.h"

void dispatchAction(SelectionAction action);
