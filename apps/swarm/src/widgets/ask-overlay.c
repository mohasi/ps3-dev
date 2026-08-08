// ask-overlay - a question in the middle of the screen, answered with a pad button.

#include "widgets/ask-overlay.h"

#include "gfx.h"
#include "pad.h"
#include "string-utilities.h"
#include "ui/console-glyphs.h"
#include "ui/image.h"   // the glyph pictures themselves belong to console-glyphs
#include "ui/label.h"
#include "widgets/theme.h"

#define DIALOG_WIDTH  760
#define DIALOG_HEIGHT 190
#define TITLE_SIZE     26
#define BUTTON_SIZE    20
#define GLYPH_SIZE     28
#define GLYPH_GAP      10   // between a glyph and its own label
#define BUTTON_GAP     44   // between one answer and the next

#define ANSWER_MAX 3

static Font *font;
static AnswerCallback onAnswer;
static int armed;   // the press that opened the dialog must not answer it as well

static Image glyphs[ANSWER_MAX];
static Label titleLabel;
static Label answerLabels[ANSWER_MAX];
static int   answerShown[ANSWER_MAX];

static int getDialogX(void) { return (1920 - DIALOG_WIDTH) / 2; }
static int getDialogY(void) { return (1080 - DIALOG_HEIGHT) / 2; }

void initAskOverlay(Font *appFont)
{
   font = appFont;

   initGlyphIcon(&glyphs[ANSWER_CROSS], GLYPH_CROSS, GLYPH_SIZE);
   initGlyphIcon(&glyphs[ANSWER_SQUARE], GLYPH_SQUARE, GLYPH_SIZE);
   initGlyphIcon(&glyphs[ANSWER_CIRCLE], GLYPH_CIRCLE, GLYPH_SIZE);

   initLabelRaw(&titleLabel, font, 0, 0, DIALOG_WIDTH - 60, AUTO, TITLE_SIZE, TEXT_BRIGHT, TEXT_NOWRAP_ELLIPSIS, "");
   for (int index = 0; index < ANSWER_MAX; index++)
      initLabel(&answerLabels[index], font, 0, 0, 300, AUTO, BUTTON_SIZE, TEXT_PLAIN, TEXT_NOWRAP, "");
}

void ask(const char *title, const char *crossText, const char *squareText, const char *circleText,
         AnswerCallback callback)
{
   const char *texts[ANSWER_MAX] = { crossText, squareText, circleText };

   setLabelText(&titleLabel, title);
   for (int index = 0; index < ANSWER_MAX; index++) {
      answerShown[index] = texts[index] != NULL;
      setLabelText(&answerLabels[index], answerShown[index] ? texts[index] : "");
   }

   onAnswer = callback;
   armed = 0;
   showOverlay(&askOverlay);
}

static void answer(Answer choice)
{
   AnswerCallback callback = onAnswer;

   hideOverlay(&askOverlay);
   if (callback) callback(choice);
}

static void showAsk(void)
{
   askOverlay.status = OVERLAY_VISIBLE;
}

static void hideAsk(void)
{
   askOverlay.status = OVERLAY_HIDDEN;
}

static void updateAsk(void)
{
   if (!armed) { armed = 1; return; }

   if (answerShown[ANSWER_CROSS] && isPadButtonPressed(PAD_BTN_CROSS)) answer(ANSWER_CROSS);
   else if (answerShown[ANSWER_SQUARE] && isPadButtonPressed(PAD_BTN_SQUARE)) answer(ANSWER_SQUARE);
   else if (answerShown[ANSWER_CIRCLE] && isPadButtonPressed(PAD_BTN_CIRCLE)) answer(ANSWER_CIRCLE);
}

// the answers as one centred row of glyph and label
static void drawAnswers(int y)
{
   int width = 0;
   for (int index = 0; index < ANSWER_MAX; index++) {
      if (!answerShown[index]) continue;
      width += GLYPH_SIZE + GLYPH_GAP + answerLabels[index].tt.tex.w + BUTTON_GAP;
   }

   int x = (1920 - (width - BUTTON_GAP)) / 2;
   for (int index = 0; index < ANSWER_MAX; index++) {
      if (!answerShown[index]) continue;

      drawImageAt(&glyphs[index], x, y);
      x += GLYPH_SIZE + GLYPH_GAP;

      moveLabel(&answerLabels[index], x, y + (GLYPH_SIZE - answerLabels[index].tt.tex.h) / 2);
      drawLabel(&answerLabels[index]);
      x += answerLabels[index].tt.tex.w + BUTTON_GAP;
   }
}

static void drawAsk(void)
{
   fillGfxRectangle(0, 0, 1920, 1080, 0xC0000000);   // the screen behind, dimmed

   int x = getDialogX(), y = getDialogY();
   drawGfxBox(x, y, DIALOG_WIDTH, DIALOG_HEIGHT, 2, PANEL_FILL, PICKED_BORDER);

   moveLabel(&titleLabel, x + (DIALOG_WIDTH - titleLabel.tt.tex.w) / 2, y + 44);
   drawLabel(&titleLabel);
   drawAnswers(y + DIALOG_HEIGHT - 70);
}

static void termAsk(void)
{
   for (int index = 0; index < ANSWER_MAX; index++) freeLabel(&answerLabels[index]);

   freeLabel(&titleLabel);
   askOverlay.status = OVERLAY_TERMINATED;
}

Overlay askOverlay = { showAsk, hideAsk, updateAsk, drawAsk, termAsk, OVERLAY_TERMINATED };
