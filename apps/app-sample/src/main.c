#include <sys/process.h>

#include "app.h"
#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "audio.h"
#include "font.h"
#include "screen.h"
#include "screens/demo.h"
#include "ui/stats.h"

#define PROCESS_PRIORITY_DEFAULT 1001
#define PROCESS_STACK_SIZE_64KB  0x10000

SYS_PROCESS_PARAM(PROCESS_PRIORITY_DEFAULT, PROCESS_STACK_SIZE_64KB)

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	appRegisterExitCallback();

	if (initGfx(GFX_VSYNC_OFF) != 0) return 1;
	if (initSfx() != 0) return 1;
	if (initFont() != 0) return 1;
	initPad();

	initStats(5, 5, 14, COLOR_AMBER_300);

	changeScreen(&demoScreen);

	while (!appExitRequested) {
		appPoll();
		updatePad();

		updateScreen();

		beginGfxFrame();
		clearGfx(COLOR_SLATE_900);
		drawScreen();
		drawStats();
		endGfxFrame();
	}

	changeScreen(NULL);
	termSfx();
	termFont();
	termGfx();
	return 0;
}
