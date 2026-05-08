#include <sys/process.h>

#include "app.h"
#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "audio.h"
#include "font.h"
#include "screen.h"
#include "screens/demo.h"
#include "overlays/stats.h"

#define PROCESS_PRIORITY_DEFAULT 1001
#define PROCESS_STACK_SIZE_64KB  0x10000

SYS_PROCESS_PARAM(PROCESS_PRIORITY_DEFAULT, PROCESS_STACK_SIZE_64KB)

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	appRegisterExitCallback();

	if (gfxInit(GFX_VSYNC_OFF) != 0) return 1;
	if (sfxInit() != 0) return 1;
	if (fontInit() != 0) return 1;
	padInit();

	overlayShow(&stats);
	changeScreen(&demoScreen);

	while (!appExitRequested) {
		appPoll();
		padUpdate();

		screenUpdate();
		overlayUpdate(&stats);

		gfxBeginFrame();
		gfxClear(COLOR_SLATE_900);
		screenDraw();
		overlayDraw(&stats);
		gfxEndFrame();
	}

	changeScreen(NULL);
	overlayTerm(&stats);
	sfxTerm();
	fontTerm();
	gfxTerm();
	return 0;
}
