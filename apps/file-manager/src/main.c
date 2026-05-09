#include <sys/process.h>

#include "app.h"
#include "gfx.h"
#include "colors.h"
#include "pad.h"
#include "audio.h"
#include "font.h"
#include "screen.h"
#include "screens/home.h"

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

	changeScreen(&homeScreen);

	while (!appExitRequested) {
		appPoll();
		padUpdate();

		screenUpdate();

		gfxBeginFrame();
		gfxClear(COLOR_BLACK);
		screenDraw();
		gfxEndFrame();
	}

	changeScreen(NULL);
	sfxTerm();
	fontTerm();
	gfxTerm();
	return 0;
}
