#pragma once

// the one settings file cell-stream shares between its shortcut bindings (shortcuts.c) and its saved
// preferences (main.c). one place so the two can never drift onto different paths.
#define CELL_STREAM_SETTINGS_DIR  "/dev_hdd0/tmp/cell-stream"
#define CELL_STREAM_SETTINGS_PATH CELL_STREAM_SETTINGS_DIR "/settings.txt"
