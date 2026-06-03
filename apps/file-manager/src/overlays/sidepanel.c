// sidepanel - slides in from the right; header + vertical action list.
#include "overlays/sidepanel.h"
#include "gfx.h"
#include "anim.h"
#include "pad.h"
#include "font.h"
#include "audio.h"
#include "colors.h"
#include "ui/label.h"
#include "ui/image.h"
#include "ui/slice.h"
#include "button-repeat.h"
#include "string-utilities.h"
#include "sprite-regions.h"

#define PANEL_WIDTH      474
#define PANEL_BORDER     2
#define SLIDE_MS         300

// header layout (panel-relative)
#define HEADER_HEIGHT    197
#define TOP_NO_HEADER    18  // top inset for the first row when no header is drawn
#define ICON_X           35
#define ICON_Y           70
#define ICON_W           70
#define ICON_H           84
#define TEXT_X           133
#define TEXT_RIGHT_PAD   8
#define TITLE_Y          75
#define SUBTITLE_Y       108
#define DETAIL_Y         136
#define TITLE_SIZE       24
#define SUBTITLE_SIZE    18
#define DETAIL_SIZE      18

// row layout (item 414x94; first row at panel-relative (18, HEADER_HEIGHT))
#define ROW_X            18
#define ROW_WIDTH        414
#define ROW_HEIGHT       94
#define ROW_ICON_OFFSET  25
#define ROW_ICON_SIZE    48
#define ROW_TEXT_X       96
#define ROW_TITLE_Y      22
#define ROW_SUBTITLE_Y   57
#define ROW_TITLE_SIZE   22
#define ROW_SUBTITLE_SIZE 16
#define HIGHLIGHT_CAP    5  // highlight sprite (14x14) 9-slice corner cap

#define COLOR_PANEL_BG     0xFF01142B
#define COLOR_PANEL_BORDER 0xFF4A566F
#define COLOR_SUBTITLE     0x80FFFFFF

static float x;
static Anims anims;
static Font  font;

static SelectionSummary       summary;
static SelectionAction        actions[SIDEPANEL_MAX_ACTIONS];
static int                    actionCount;
static int                    selectedIndex;
static int                    hasHeader;
static SelectionActionHandler actionHandler;

static Label headerTitle, headerSubtitle, headerDetail;
static Label rowTitles[SIDEPANEL_MAX_ACTIONS];
static Label rowSubtitles[SIDEPANEL_MAX_ACTIONS];
static Image headerIcon;
static Image rowIcons[SIDEPANEL_MAX_ACTIONS];
static NineSlice hover;
static GfxTexture spritesheet;
static Audio *clickSfx;

void initSidepanel(GfxTexture sprites, Audio *sfx, SelectionActionHandler handler)
{
    spritesheet   = sprites;
    clickSfx      = sfx;
    actionHandler = handler;

    initNineSlice(&hover, sprites, 0, 0, ROW_WIDTH, ROW_HEIGHT, spriteRegions[SPRITE_HIGHLIGHT], HIGHLIGHT_CAP, HIGHLIGHT_CAP);

    font = openSystemFont(FONT_POP);
    int headerLabelW = PANEL_WIDTH - TEXT_X    - TEXT_RIGHT_PAD;
    int rowLabelW    = ROW_WIDTH   - ROW_TEXT_X - TEXT_RIGHT_PAD;
    initLabel(&headerTitle,    &font, 0, 0, headerLabelW, AUTO, TITLE_SIZE,    COLOR_WHITE,    TEXT_NOWRAP_ELLIPSIS, "");
    initLabel(&headerSubtitle, &font, 0, 0, headerLabelW, AUTO, SUBTITLE_SIZE, COLOR_SUBTITLE, TEXT_NOWRAP,          "");
    initLabel(&headerDetail,   &font, 0, 0, headerLabelW, AUTO, DETAIL_SIZE,   COLOR_SUBTITLE, TEXT_NOWRAP,          "");
    for (int i = 0; i < SIDEPANEL_MAX_ACTIONS; i++) {
        initLabel(&rowTitles[i],    &font, 0, 0, rowLabelW, AUTO, ROW_TITLE_SIZE,    COLOR_WHITE,    TEXT_NOWRAP_ELLIPSIS, "");
        initLabel(&rowSubtitles[i], &font, 0, 0, rowLabelW, AUTO, ROW_SUBTITLE_SIZE, COLOR_SUBTITLE, TEXT_NOWRAP_ELLIPSIS, "");
    }
}

void setSidepanelContent(const SelectionSummary *s, const SelectionAction *a, int count)
{
    if (count > SIDEPANEL_MAX_ACTIONS) count = SIDEPANEL_MAX_ACTIONS;
    actionCount = count;
    for (int i = 0; i < count; i++) actions[i] = a[i];
    selectedIndex = 0;

    for (int i = 0; i < actionCount; i++) {
        setLabelText(&rowTitles[i],    getActionTitle(actions[i]));
        setLabelText(&rowSubtitles[i], getActionSubtitle(actions[i]));
        initImage(&rowIcons[i], spritesheet, 0, 0, ROW_ICON_SIZE, ROW_ICON_SIZE, getActionIcon(actions[i]), GFX_FILTER_LINEAR);
    }

    // snapshot the header from the summary as it is right now; the panel
    // does not refresh while open even if file-list keeps sizing folders.
    summary = *s;
    hasHeader = summary.title && summary.title[0];

    char upper[LABEL_MAX_TEXT];
    toUpper(upper, (int)sizeof(upper), strOrEmpty(summary.title));
    setLabelText(&headerTitle,    upper);
    setLabelText(&headerSubtitle, strOrEmpty(summary.subtitle));
    setLabelText(&headerDetail,   strOrEmpty(summary.detail));

    initImage(&headerIcon, spritesheet, 0, 0, ICON_W, ICON_H, summary.icon, GFX_FILTER_LINEAR);
}

static void show(void)
{
    int sw = getGfxScreenWidth();
    x = (float)sw;
    setAnim(&anims, &x, (float)sw, (float)(sw - PANEL_WIDTH), SLIDE_MS, EASE_OUT_CUBIC, ANIM_ONCE, NULL);
    sidepanel.status = OVERLAY_VISIBLE;
}

static void onHidden(AnimHandle self) { (void)self; sidepanel.status = OVERLAY_HIDDEN; }

static void hide(void)
{
    setAnim(&anims, &x, x, (float)getGfxScreenWidth(), SLIDE_MS, EASE_IN_CUBIC, ANIM_ONCE, onHidden);
}

static void handleInput(void)
{
    if (pad.btn.triangle == BTN_PRESSED || pad.btn.circle == BTN_PRESSED) {
        hideOverlay(&sidepanel);
        return;
    }
    if (actionCount == 0) return;
    static ButtonRepeat repeat;
    if (buttonRepeated(&repeat, pad.btn.up)) {
        selectedIndex = (selectedIndex - 1 + actionCount) % actionCount;
        playSfxOnce(clickSfx);
    }
    else if (buttonRepeated(&repeat, pad.btn.down)) {
        selectedIndex = (selectedIndex + 1) % actionCount;
        playSfxOnce(clickSfx);
    }
    if (pad.btn.cross == BTN_PRESSED) {
        SelectionAction picked = actions[selectedIndex];
        hideOverlay(&sidepanel);
        if (actionHandler) actionHandler(picked);
    }
}

static void update(void)
{
    handleInput();
    updateAnim(&anims);
}

static void draw(void)
{
    int px = (int)x;
    int sw = getGfxScreenWidth();
    int sh = getGfxScreenHeight();

    fillGfxRectangle(px, 0, sw - px, sh, COLOR_PANEL_BG);
    fillGfxRectangle(px, 0, PANEL_BORDER, sh, COLOR_PANEL_BORDER);

    if (hasHeader) {
        drawImageAt(&headerIcon, px + ICON_X, ICON_Y);
        drawLabelAt(&headerTitle,    px + TEXT_X, TITLE_Y);
        drawLabelAt(&headerSubtitle, px + TEXT_X, SUBTITLE_Y);
        drawLabelAt(&headerDetail,   px + TEXT_X, DETAIL_Y);
    }

    int rowX = px + ROW_X;
    int rowOriginY = hasHeader ? HEADER_HEIGHT : TOP_NO_HEADER;
    for (int i = 0; i < actionCount; i++) {
        int rowY = rowOriginY + i * ROW_HEIGHT;
        if (i == selectedIndex) {
            moveNineSlice(&hover, rowX, rowY);
            drawNineSlice(&hover);
        }
        drawImageAt(&rowIcons[i],    rowX + ROW_ICON_OFFSET, rowY + ROW_ICON_OFFSET);
        drawLabelAt(&rowTitles[i],    rowX + ROW_TEXT_X, rowY + ROW_TITLE_Y);
        drawLabelAt(&rowSubtitles[i], rowX + ROW_TEXT_X, rowY + ROW_SUBTITLE_Y);
    }
}

static void term(void)
{
    cancelAllAnims(&anims);
    closeFont(&font);
    sidepanel.status = OVERLAY_TERMINATED;
}

Overlay sidepanel = { show, hide, update, draw, term, OVERLAY_TERMINATED };