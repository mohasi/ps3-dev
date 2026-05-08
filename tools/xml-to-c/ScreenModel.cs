using System.Collections.Generic;

namespace XmlToC
{
    // Parsed representation of a <screen> XML document.
    // Just data: parsing/validation lives in XmlParser, emission in CodeGenerator.

    internal enum ElementKind
    {
        Font,
        Texture,
        Rectangle,
        Circle,
        Triangle,
        Line,
        Image,
        Label,
        Breadcrumb
    }

    internal abstract class Element
    {
        public string Id;          // unique within screen, valid C identifier (used in code)
        public string Name;        // human-readable label (used in comments)
        public ElementKind Kind;

        // raw XML comments that immediately preceded this element. Each entry
        // is one <!-- ... --> block; multi-line blocks keep their newlines and
        // are split into // lines at emit time.
        public List<string> LeadingComments = new List<string>();
    }

    // ---- resources (declared, not drawn) ----

    internal sealed class FontElement : Element
    {
        public string SystemType;  // FONT_POP / FONT_GOTHIC_JP / FONT_SANS / FONT_SERIF
    }

    internal sealed class TextureElement : Element
    {
        public string Path;        // raw path string, escaped at emit time
    }

    // ---- ui components ----

    internal sealed class RectangleElement : Element
    {
        public int X, Y, Width, Height;
        public int Radius;            // 0 -> sharp corners, >0 -> rounded
        public string Fill;
        public int BorderThickness;   // 0 -> no border
        public string BorderColor;    // null -> COLOR_TRANSPARENT
    }

    internal sealed class CircleElement : Element
    {
        public int Cx, Cy, Radius;
        public string Fill;
        public int BorderThickness;
        public string BorderColor;
    }

    internal sealed class TriangleElement : Element
    {
        public float X0, Y0, X1, Y1, X2, Y2;
        public string Fill;
        public int BorderThickness;
        public string BorderColor;
    }

    internal sealed class LineElement : Element
    {
        public int X0, Y0, X1, Y1;
        public int Thickness;
        public string Color;
    }

    internal sealed class ImageElement : Element
    {
        public string TextureRef;  // name of a <texture> element
        public int X, Y;
        public int Width, Height;  // 0 -> AUTO (use native texture size)
        public string Filter;      // GFX_FILTER_LINEAR / GFX_FILTER_NEAREST
        public int TexX, TexY, TexW, TexH; // spritesheet region (all 0 -> full texture)
    }

    internal sealed class LabelElement : Element
    {
        public int X, Y;
        public int Width, Height;  // 0 -> AUTO
        public int Size;
        public string Color;
        public string FontRef;     // name of a <font> element
        public string Content;     // initial text (may be empty)
        public string Wrap;        // TEXT_WRAP / TEXT_NOWRAP / TEXT_NOWRAP_ELLIPSIS
    }

    internal sealed class BreadcrumbSegment
    {
        public string Text;
    }

    internal sealed class BreadcrumbElement : Element
    {
        public int X, Y, Width, Height;
        public string FontRef;
        public string BgColor;
        public string BorderColor;
        public string TextColor;
        public string ChevronColor;
        public int BorderRadius;
        public int BorderThickness;
        public int FontSize;
        public List<BreadcrumbSegment> Segments = new List<BreadcrumbSegment>();
    }

    internal sealed class ScreenModel
    {
        public string Name;
        public string Fill;        // optional; null/empty -> no gfxClear emitted

        // separated for cleaner emit ordering: open fonts first, then load textures.
        // Drawables run in document order in the draw fn.
        public List<FontElement>    Fonts     = new List<FontElement>();
        public List<TextureElement> Textures  = new List<TextureElement>();
        public List<Element>        Drawables = new List<Element>();
    }
}
