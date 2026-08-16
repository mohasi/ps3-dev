#pragma once

// PAF widget primitives: the firmware calls and data layout needed to draw over a
// running game through vsh's compositor. Shared by the cheat menu panel (overlay.cpp)
// and the always-on stats counter (stats-overlay.cpp).
//
// A PAF widget is a C++ object whose second field is a std::string (its name). PAF
// walks that string every frame, so the widget must be a properly constructed C++
// object — constructing into a zeroed buffer leaves the string's pointer null and
// faults the walk (that was the hard-lock). Hence C++, and hence makeTextWidget /
// makePlaneWidget placement-new before handing the object to PAF's own constructor.
// We never set a widget name, so that string stays empty and never allocates (libc
// malloc doesn't resolve in a vsh prx).
//
// Two rules learned on hardware (FREEZE-WRITEUP.md):
// - never reuse a widget pointer across the XMB/game boundary; re-find the parent
//   per use, on the frame thread, and rebuild when it changed.
// - any string crossing into firmware must use the Dinkumware SSO layout with
//   16-bit chars (FirmwareWstring), never our GCC COW strings.

#include <string>

// PhText storage is bigger than a plain widget: PhWidget 0x290 + PhText's 0x14 extra.
#define PHTEXT_SIZE  0x2A4

// horizontal alignment (PhWidget::SetStyle int overload). 0x31 justifies the glyphs,
// 0x12 moves the anchor to the matching edge so the position x is that edge; set both.
#define PAF_ALIGN_CENTER  0
#define PAF_ALIGN_LEFT    1
#define PAF_ALIGN_RIGHT   2
#define PAF_ANCHOR_TOP    16
#define PAF_ANCHOR_BOTTOM 32

// paf exports, resolved by NID from libpaf_export_stub.a. the asm label binds
// each readable name to its NID symbol; the trailing comment gives the real
// firmware method for cross-referencing community headers.
extern "C" {
   uint32_t findPafView(const char *pluginName)                              __asm__("paf_F21655F3"); // paf::View::Find
   uint32_t findPafViewWidget(uint32_t view, const char *widgetName)         __asm__("paf_794CEACB"); // paf::View::FindWidget
   void     constructPafPlane(void *plane, void *parent, void *appear)       __asm__("paf_D0197A7D"); // paf::PhPlane::PhPlane
   void     constructPafText(void *text, void *parent, void *appear)         __asm__("paf_7F0930C6"); // paf::PhText::PhText
   void     updatePafLayoutPos(void *widget)                                 __asm__("paf_BF4B155C"); // paf::PhWidget::UpdateLayoutPos
   void     updatePafLayoutSize(void *widget)                                __asm__("paf_DF031EDD"); // paf::PhWidget::UpdateLayoutSize
   int      killPafTimerCallback(void *handler, int callbackId)              __asm__("paf_2CBA5A33"); // paf::PhHandler::KillTimerCB
   void     setPafTextStyleInt(void *textRender, int style, int value)       __asm__("paf_983EA578"); // paf::PhSText::SetStyle(int, int)
   void     setPafTextStyleFloat(void *textRender, int style, float value)   __asm__("paf_165AD4A6"); // paf::PhSText::SetStyle(int, float)
   void     preparePafWidgetUpdate(void *widget)                             __asm__("paf_384F93FC"); // paf::PhWidget::UpdatePrepare
   void    *getPafPluginInterface(uint32_t view, int identifier)             __asm__("paf_23AFB290"); // paf plugin GetInterface
   void    *findPafChildWidget(void *widget, const char *childName, int unused) __asm__("paf_D557F850"); // paf::PhWidget::FindChild
   void     destructPafWidget(void *widget)                                  __asm__("paf_738BAAC0"); // paf::PhWidget::~PhWidget
   uint32_t getPafDrawSurfaceWidth(void)                                     __asm__("paf_F476E8AA"); // pafGuGetDrawSurfW
   uint32_t getPafDrawSurfaceHeight(void)                                    __asm__("paf_AC984A12"); // pafGuGetDrawSurfH
   float    getPafTextWidth(void *text)                                      __asm__("paf_65036474"); // paf::PhText::GetTextWidth
}

// the console's OWN allocator, exported by sysPrxForUser and resolved from liblv2_stub.a. This
// is NOT libc malloc, which does not resolve in a vsh prx. Widgets must come from here: the
// firmware frees the children of a page when it tears that page down, so it has to be memory
// the firmware handed out. This is what the reference's new/delete override calls.
extern "C" void *_sys_malloc(unsigned int size);
extern "C" void  _sys_free(void *ptr);

// Minimal mirror of vsh's PhWidget data layout. We name only the fields we touch;
// everything else is opaque bytes the firmware constructor fills — including the
// widget's real name string at 0x004 (a 0x1C-byte Dinkumware std::string, not our
// GCC one; our member is effectively padding and pad0 is sized from sizeof(std::string)
// so the offsets we use stay exact).
struct PafWidget {
   void        *vtable;                                        // 0x000
   std::string  name;                                          // 0x004
   char         pad0[0x0E0 - 0x004 - sizeof(std::string)];     // .. 0x0E0
   PafWidget   *parent;                                        // 0x0E0 set by the firmware constructor
   char         pad0a[0x0F0 - 0x0E4];                          // .. 0x0F0
   void        *renderHandle;                                  // 0x0F0 sRender; PhSText at +0x28
   char         pad0b[0x120 - 0x0F4];                          // .. 0x120
   float        colorScaleRGBA[4];                             // 0x120
   char         pad1[0x250 - 0x130];                           // .. 0x250
   int          positionFactor[3];                             // 0x250 x,y,z
   float        positionLayout[4];                             // 0x25C vec4
   int          sizeFactor[3];                                 // 0x26C x,y,z
   float        sizeLayout[4];                                 // 0x278 vec4
   char         pad2[0x290 - 0x288];                           // .. 0x290
};

// system_plugin's page_notification, the widget everything is parented to. vsh tears
// it down and rebuilds it across a game change, so callers re-find it on the frame
// thread and rebuild their widgets when the address changed. 0 = not up yet.
void *findPageNotification(void);

// construct a widget into caller-owned storage, parented to page_notification.
// text needs PHTEXT_SIZE bytes, a plane needs sizeof(PafWidget). colour is not set:
// a fresh widget is invisible until the caller colours it.
PafWidget *makeTextWidget(void *storage, void *parent, const char *text, float x, float y, float height, int align);
PafWidget *makePlaneWidget(void *storage, void *parent, float x, float y, float width, float height);

// A widget carries its own name and its own parent pointer. Both are needed to answer the only
// question that matters before touching it: is it still a child of the parent it was built
// under? The firmware unregisters children as it tears a page down, so a widget that answers no
// is already dead and must not be written to. Names are at most 15 characters, which keeps them
// in the string's inline storage so naming never allocates.
#define PAF_WIDGET_NAME_MAX  15
void setPafWidgetName(PafWidget *widget, const char *name);
int  isPafWidgetAttached(PafWidget *widget, const char *name);

// unregister the widget from its parent and give its memory back. Only safe while the widget is
// still attached; an unattached one is already gone and must simply be forgotten.
void destroyPafWidget(PafWidget *widget);

// Move the point a widget's position refers to. A widget anchored at its bottom grows upward when
// its height changes, so a bar chart only has to write the height and not the position too.
void setPafWidgetAnchor(PafWidget *widget, int anchor);

void setPafWidgetText(PafWidget *widget, const char *text);
void setPafWidgetPosition(PafWidget *widget, float x, float y);
void setPafWidgetSize(PafWidget *widget, float width, float height);
void setPafWidgetColor(PafWidget *widget, float red, float green, float blue, float alpha);
