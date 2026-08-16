#include "paf-widget.h"
#include "string-utilities.h"   // memSet + utf8ToUtf16: libc-free, and libc doesn't resolve in a vsh prx

#include <new>
#include <stdint.h>

// SetText is virtual: (this, const std::wstring &, 0), where the wstring is the
// FIRMWARE's std::wstring — the SDK's Dinkumware SSO layout (0x1C bytes: 4-byte
// allocator pad, 16-byte union of 8 inline 16-bit chars / heap pointer, length
// at 0x14, capacity at 0x18; wchar_t on PS3 is 16-bit per sdk yvals.h). NOT the
// GCC COW single-pointer string — passing that froze the console (SetText read
// length/capacity from garbage past the object and did a wild copy).
typedef void (*PafSetTextFn)(void *widget, const void *fwWstring, int unused);

// two modes by capacity: < 8 keeps the text in the inline chars; >= 8 makes
// the 0x04 union a pointer to a char buffer (see setPafWidgetText). the union
// makes the pointer a real member — no type-punning the char array.
struct FirmwareWstring {
   unsigned int allocatorPad;         // 0x00
   union {
      unsigned short  inlineChars[8]; // 0x04  SSO text when inline
      unsigned short *heapBuffer;     // 0x04  buffer pointer when heap
   } text;
   unsigned int length;               // 0x14  _Mysize
   unsigned int capacity;             // 0x18  _Myres; < 8 = inline, >= 8 = heap
};

// the widget's own name is the FIRMWARE's std::string at offset 0x004 — the same Dinkumware
// layout as the wstring above with 8-bit chars: 4-byte allocator pad, 16-byte union of inline
// characters or a heap pointer, length at 0x14, capacity at 0x18. The SDK's own string code
// (xstring, _Tidy) frees the buffer only when the capacity reaches 16, so a capacity of 15 keeps
// the text inline and means naming a widget neither allocates nor leaves anything to free.
struct FirmwareString {
   unsigned int allocatorPad;      // 0x00
   union {
      char  inlineChars[16];       // 0x04  text when inline
      char *heapBuffer;            // 0x04  buffer pointer when heap
   } text;
   unsigned int length;            // 0x14  _Mysize
   unsigned int capacity;          // 0x18  _Myres; 16 or more means heap
};

#define PAF_WIDGET_NAME_OFFSET  0x04

#define PAF_COLOR_HANDLER      0x1000002   // PhHandler::ColorHandler
#define PAF_VTABLE_SET_TEXT    70          // PhText::SetText slot (from the reference)

// text render styles (PhSText::SetStyle). 0x28 is line height per the reference;
// 0x13=0x70 is set by the reference at every text widget's creation, meaning
// undocumented — text stays invisible without both.
#define PAF_STYLE_TEXT_SETUP   0x13
#define PAF_STYLE_LINE_HEIGHT  0x28
#define PAF_STYLE_TEXT_ALIGN   0x31
#define PAF_STYLE_ANCHOR       0x12

namespace {

// shared scratch for building a firmware wstring of any length. SetText copies
// the source synchronously into the widget's own string, so one scratch buffer
// serves every call. wchar_t on ps3 is 16-bit, so the buffer is 16-bit chars.
unsigned short  textScratch[128];
FirmwareWstring textSource;

}

void *findPageNotification(void)
{
   uint32_t view = findPafView("system_plugin");
   if (!view) return 0;
   return (void *)(uintptr_t)findPafViewWidget(view, "page_notification");
}

// set a widget's text from a UTF-8 C string, decoded to 16-bit units via the shared
// utf8ToUtf16 (wchar_t is 16-bit on ps3, and game titles are UTF-8 with the odd
// ® / ™ — decoding avoids mojibake). <=7 units use the Dinkumware SSO inline union;
// longer strings put a pointer to our scratch in the union with capacity >= 8 to
// select heap mode (SetText then copies via vsh's allocator, never ours). astral code
// points become a surrogate pair; malformed bytes are dropped.
void setPafWidgetText(PafWidget *widget, const char *text)
{
   const int maxChars = (int)(sizeof(textScratch) / sizeof(textScratch[0])) - 1;
   utf8ToUtf16(text, textScratch, maxChars);
   int length = 0;
   while (textScratch[length]) length++;

   textSource.allocatorPad = 0;
   textSource.length = length;
   if (length <= 7) {
      for (int i = 0; i <= length; i++) textSource.text.inlineChars[i] = textScratch[i];
      textSource.capacity = 7;      // < 8 selects inline mode
   } else {
      textSource.text.heapBuffer = textScratch;   // union pointer at 0x04
      textSource.capacity = length;   // >= 8 selects heap mode
   }
   ((PafSetTextFn)((void **)widget->vtable)[PAF_VTABLE_SET_TEXT])(widget, &textSource, 0);
}

// the name is set after construction, because the constructor registers the child under an empty
// one. A name too long to stay inline is refused rather than truncated into a heap string we
// would then own.
void setPafWidgetName(PafWidget *widget, const char *name)
{
   FirmwareString value;
   int length = 0;
   while (name[length] && length < PAF_WIDGET_NAME_MAX) { value.text.inlineChars[length] = name[length]; length++; }
   if (name[length]) return;
   for (int i = length; i < 16; i++) value.text.inlineChars[i] = 0;
   value.allocatorPad = 0;
   value.length = length;
   value.capacity = 15;

   char *field = (char *)widget + PAF_WIDGET_NAME_OFFSET;
   for (unsigned int i = 0; i < sizeof(value); i++) field[i] = ((const char *)&value)[i];
}

// Ask the parent the widget itself recorded, not a parent looked up fresh: a fresh lookup can
// hand back a live page while this widget's own page is already being destroyed, which is what
// made an earlier version of this check useless.
int isPafWidgetAttached(PafWidget *widget, const char *name)
{
   if (!widget || !widget->parent) return 0;
   return findPafChildWidget(widget->parent, name, 0) == (void *)widget;
}

void setPafWidgetAnchor(PafWidget *widget, int anchor)
{
   void *renderHandle = widget ? widget->renderHandle : 0;
   if (!renderHandle) return;

   typedef void (*StyleIntFn)(void *sRender, int style, int value);
   void **renderVtable = *(void ***)renderHandle;
   ((StyleIntFn)renderVtable[12])(renderHandle, PAF_STYLE_ANCHOR, anchor);
   preparePafWidgetUpdate(widget);
}

void destroyPafWidget(PafWidget *widget)
{
   if (!widget) return;
   destructPafWidget(widget);
   _sys_free(widget);
}

// direct m_Data writes + the matching update export — the reference's
// SetPosition/SetSize/SetColor idiom (factors 6,5,0 = viewport coords,
// colour committed by killing paf's colour animation timer).
void setPafWidgetPosition(PafWidget *widget, float x, float y)
{
   widget->positionFactor[0] = 6; widget->positionFactor[1] = 5; widget->positionFactor[2] = 0;
   widget->positionLayout[0] = x; widget->positionLayout[1] = y; widget->positionLayout[2] = 0.0f; widget->positionLayout[3] = 0.0f;
   updatePafLayoutPos(widget);
}

void setPafWidgetSize(PafWidget *widget, float width, float height)
{
   widget->sizeFactor[0] = 6; widget->sizeFactor[1] = 5; widget->sizeFactor[2] = 0;
   widget->sizeLayout[0] = width; widget->sizeLayout[1] = height; widget->sizeLayout[2] = 0.0f; widget->sizeLayout[3] = 0.0f;
   updatePafLayoutSize(widget);
}

void setPafWidgetColor(PafWidget *widget, float red, float green, float blue, float alpha)
{
   widget->colorScaleRGBA[0] = red; widget->colorScaleRGBA[1] = green; widget->colorScaleRGBA[2] = blue;
   widget->colorScaleRGBA[3] = alpha;
   killPafTimerCallback(widget, PAF_COLOR_HANDLER);
}

// construct a PhText into caller storage with its text, size and position. the
// render styles (0x13=0x70 setup + 0x28 height) are required or the text is invisible.
PafWidget *makeTextWidget(void *storage, void *parent, const char *text, float x, float y, float height, int align)
{
   memSet(storage, 0, PHTEXT_SIZE);   // clear any stale prior widget in this storage
   PafWidget *widget = new (storage) PafWidget;   // default-init: memSet already zeroed; no redundant zero-fill
   constructPafText(widget, parent, 0);
   setPafWidgetText(widget, text);

   void *renderHandle = widget->renderHandle;
   void *textRender = renderHandle ? *(void **)((char *)renderHandle + 0x28) : 0;
   if (textRender) {
      setPafTextStyleInt(textRender, PAF_STYLE_TEXT_SETUP, 0x70);
      setPafTextStyleFloat(textRender, PAF_STYLE_LINE_HEIGHT, height);
   }

   // horizontal alignment (only when non-center, so the proven centered path for
   // the header/subtitle is untouched). PhWidget::SetStyle(int,int) dispatches to
   // the render object's vtable method 12 with (sRender, style, value), and
   // renderHandle IS sRender - matches the VshFpsCounter reference.
   if (renderHandle && align != PAF_ALIGN_CENTER) {
      typedef void (*StyleIntFn)(void *sRender, int style, int value);
      void **renderVtable = *(void ***)renderHandle;
      StyleIntFn setStyle = (StyleIntFn)renderVtable[12];
      setStyle(renderHandle, PAF_STYLE_TEXT_ALIGN, align);
      setStyle(renderHandle, PAF_STYLE_ANCHOR, align);
   }
   preparePafWidgetUpdate(widget);
   setPafWidgetPosition(widget, x, y);
   return widget;
}

// construct a PhPlane into caller storage at a position/size. colour is applied by
// the caller. used for the dimmer, the highlight bar, the panel box and graph bars.
PafWidget *makePlaneWidget(void *storage, void *parent, float x, float y, float width, float height)
{
   memSet(storage, 0, sizeof(PafWidget));   // clear any stale prior widget
   PafWidget *widget = new (storage) PafWidget;   // default-init: memSet already zeroed; no redundant zero-fill
   constructPafPlane(widget, parent, 0);
   setPafWidgetPosition(widget, x, y);
   setPafWidgetSize(widget, width, height);
   return widget;
}
