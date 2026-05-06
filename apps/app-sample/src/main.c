#include <sys/process.h>
#include <sysutil/sysutil_common.h>

#include "gfx.h"
#include "colors.h"
#include "pad-input.h"
#include "audio.h"
#include "font.h"
#include "screen.h"
#include "screens/demo.h"
#include "overlays/stats.h"

SYS_PROCESS_PARAM(1001, 0x10000)

static volatile int exitRequested = 0;

static void exitCallback(uint64_t status, uint64_t param, void *userdata)
{
	(void)param; (void)userdata;
	if (status == CELL_SYSUTIL_REQUEST_EXITGAME) exitRequested = 1;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	cellSysutilRegisterCallback(0, exitCallback, NULL);

	if (gfxInit(GFX_VSYNC_OFF) != 0) return 1;
	if (sfxInit() != 0) return 1;
	if (fontInit() != 0) return 1;
	padInit();

	overlayShow(&stats);
	changeScreen(&demoScreen);

	while (!exitRequested) {
		cellSysutilCheckCallback();
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
