using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.RegularExpressions;
using System.Xml;
using System.Xml.Linq;

namespace XmlToC
{
    internal sealed class ParseException : Exception
    {
        public ParseException(string message) : base(message) { }
    }

    internal static class XmlParser
    {
        private static readonly Regex CIdentifier = new Regex(@"^[A-Za-z_][A-Za-z0-9_]*$", RegexOptions.Compiled);
        private static readonly Regex HexColor    = new Regex(@"^0x[0-9A-Fa-f]{6,8}$", RegexOptions.Compiled);
        private static readonly Regex ConstColor  = new Regex(@"^[A-Z][A-Z0-9_]*$", RegexOptions.Compiled);

        private static readonly HashSet<string> KnownFontTypes = new HashSet<string>
        {
            "FONT_POP", "FONT_GOTHIC_JP", "FONT_SANS", "FONT_SERIF"
        };

        private static readonly Dictionary<string, string> WrapMap = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            { "wrap",     "TEXT_WRAP" },
            { "nowrap",   "TEXT_NOWRAP" },
            { "ellipsis", "TEXT_NOWRAP_ELLIPSIS" }
        };

        private static readonly Dictionary<string, string> FilterMap = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            { "linear",  "GFX_FILTER_LINEAR" },
            { "nearest", "GFX_FILTER_NEAREST" }
        };

        public static ScreenModel Parse(string path)
        {
            XDocument doc;
            try
            {
                doc = XDocument.Load(path, LoadOptions.SetLineInfo);
            }
            catch (XmlException ex)
            {
                throw new ParseException("XML is not well-formed: " + ex.Message);
            }

            var root = doc.Root;
            if (root == null || root.Name.LocalName != "screen")
                throw new ParseException("Root element must be <screen>.");

            var model = new ScreenModel
            {
                Name = RequireAttr(root, "name"),
                Fill = OptAttr(root, "fill")
            };

            if (!CIdentifier.IsMatch(model.Name))
                throw new ParseException(At(root) + "screen name '" + model.Name + "' is not a valid C identifier.");

            if (!string.IsNullOrEmpty(model.Fill))
            {
                ValidateColor(root, "fill", model.Fill);
                model.Fill = NormalizeColor(model.Fill);
            }

            var seenNames = new HashSet<string>(StringComparer.Ordinal);
            var pendingComments = new List<string>();

            foreach (var node in root.Nodes())
            {
                var comment = node as XComment;
                if (comment != null)
                {
                    pendingComments.Add(comment.Value);
                    continue;
                }

                var el = node as XElement;
                if (el == null) continue;

                Element parsed = ParseElement(el);

                if (!string.IsNullOrEmpty(parsed.Id))
                {
                    if (!CIdentifier.IsMatch(parsed.Id))
                        throw new ParseException(At(el) + "id '" + parsed.Id + "' is not a valid C identifier.");
                    if (!seenNames.Add(parsed.Id))
                        throw new ParseException(At(el) + "duplicate id '" + parsed.Id + "'.");
                }

                if (pendingComments.Count > 0)
                {
                    parsed.LeadingComments.AddRange(pendingComments);
                    pendingComments.Clear();
                }

                if (parsed is FontElement)
                    model.Fonts.Add((FontElement)parsed);
                else if (parsed is TextureElement)
                    model.Textures.Add((TextureElement)parsed);
                else
                    model.Drawables.Add(parsed);
            }

            // cross-reference resolution
            var fontIds    = new HashSet<string>(model.Fonts.Select(f => f.Id), StringComparer.Ordinal);
            var textureIds = new HashSet<string>(model.Textures.Select(t => t.Id), StringComparer.Ordinal);

            foreach (var el in root.Elements())
            {
                switch (el.Name.LocalName)
                {
                    case "Label":
                    case "Breadcrumb":
                        {
                            XAttribute fa = el.Attribute("font-id");
                            string fref = fa != null ? fa.Value : null;
                            if (fref != null && !fontIds.Contains(fref))
                                throw new ParseException(At(el) + "<" + el.Name.LocalName + "> references font '" + fref + "', which is not declared.");
                            break;
                        }
                    case "Image":
                        {
                            XAttribute ta = el.Attribute("texture-id");
                            string tref = ta != null ? ta.Value : null;
                            if (tref != null && !textureIds.Contains(tref))
                                throw new ParseException(At(el) + "<Image> references texture '" + tref + "', which is not declared.");
                            break;
                        }
                }
            }

            return model;
        }

        private static Element ParseElement(XElement el)
        {
            switch (el.Name.LocalName)
            {
                case "Font":        return ParseFont(el);
                case "Texture":     return ParseTexture(el);
                case "Rectangle":   return ParseRectangle(el);
                case "Circle":      return ParseCircle(el);
                case "Triangle":    return ParseTriangle(el);
                case "Line":        return ParseLine(el);
                case "Image":       return ParseImage(el);
                case "Label":       return ParseLabel(el);
                case "Breadcrumb":  return ParseBreadcrumb(el);
                default:
                    throw new ParseException(At(el) + "unknown element <" + el.Name.LocalName + ">.");
            }
        }

        // ---- resources ----

        private static FontElement ParseFont(XElement el)
        {
            string type = RequireAttr(el, "type");
            if (!KnownFontTypes.Contains(type))
                throw new ParseException(At(el) + "font type '" + type + "' must be one of: " + Join(KnownFontTypes) + ".");

            return new FontElement
            {
                Kind       = ElementKind.Font,
                Id         = RequireAttr(el, "id"),
                Name       = OptAttr(el, "name"),
                SystemType = type
            };
        }

        private static TextureElement ParseTexture(XElement el)
        {
            return new TextureElement
            {
                Kind = ElementKind.Texture,
                Id   = RequireAttr(el, "id"),
                Name = OptAttr(el, "name"),
                Path = RequireAttr(el, "path")
            };
        }

        // ---- shapes ----

        private static RectangleElement ParseRectangle(XElement el)
        {
            int x, y; ParseIntPair(el, "xy", out x, out y);
            int w, h; ParseIntPair(el, "wh", out w, out h);
            string bc = OptAttr(el, "border-color");
            if (bc != null) { ValidateColor(el, "border-color", bc); bc = NormalizeColor(bc); }
            return new RectangleElement
            {
                Kind            = ElementKind.Rectangle,
                Id              = RequireAttr(el, "id"),
                Name            = OptAttr(el, "name"),
                X               = x,
                Y               = y,
                Width           = w,
                Height          = h,
                Radius          = ParseAutoOrInt(el, "corner-radius", 0),
                Fill            = ParseColor(el, "fill"),
                BorderThickness = ParseAutoOrInt(el, "border-thickness", 0),
                BorderColor     = bc
            };
        }

        private static CircleElement ParseCircle(XElement el)
        {
            int cx, cy; ParseIntPair(el, "xy", out cx, out cy);
            string bc = OptAttr(el, "border-color");
            if (bc != null) { ValidateColor(el, "border-color", bc); bc = NormalizeColor(bc); }
            return new CircleElement
            {
                Kind            = ElementKind.Circle,
                Id              = RequireAttr(el, "id"),
                Name            = OptAttr(el, "name"),
                Cx              = cx,
                Cy              = cy,
                Radius          = ParseInt(el, "radius"),
                Fill            = ParseColor(el, "fill"),
                BorderThickness = ParseAutoOrInt(el, "border-thickness", 0),
                BorderColor     = bc
            };
        }

        private static TriangleElement ParseTriangle(XElement el)
        {
            float x0, y0; ParseFloatPair(el, "p0", out x0, out y0);
            float x1, y1; ParseFloatPair(el, "p1", out x1, out y1);
            float x2, y2; ParseFloatPair(el, "p2", out x2, out y2);
            string bc = OptAttr(el, "border-color");
            if (bc != null) { ValidateColor(el, "border-color", bc); bc = NormalizeColor(bc); }
            return new TriangleElement
            {
                Kind            = ElementKind.Triangle,
                Id              = RequireAttr(el, "id"),
                Name            = OptAttr(el, "name"),
                X0              = x0,
                Y0              = y0,
                X1              = x1,
                Y1              = y1,
                X2              = x2,
                Y2              = y2,
                Fill            = ParseColor(el, "fill"),
                BorderThickness = ParseAutoOrInt(el, "border-thickness", 0),
                BorderColor     = bc
            };
        }

        private static LineElement ParseLine(XElement el)
        {
            int x0, y0, x1, y1;
            ParseIntPair(el, "from", out x0, out y0);
            ParseIntPair(el, "to",   out x1, out y1);
            return new LineElement
            {
                Kind      = ElementKind.Line,
                Id        = RequireAttr(el, "id"),
                Name      = OptAttr(el, "name"),
                X0        = x0,
                Y0        = y0,
                X1        = x1,
                Y1        = y1,
                Thickness = ParseInt(el, "thickness"),
                Color     = ParseColor(el, "color")
            };
        }

        // ---- ui components ----

        private static ImageElement ParseImage(XElement el)
        {
            string filterInput = OptAttr(el, "filter") ?? "nearest";
            string filter;
            if (!FilterMap.TryGetValue(filterInput, out filter))
                throw new ParseException(At(el) + "filter '" + filterInput + "' must be one of: " + Join(FilterMap.Keys) + ".");

            int x, y; ParseIntPair(el, "xy", out x, out y);
            int w, h; ParseOptIntPair(el, "wh", 0, out w, out h);
            int sx, sy; ParseOptIntPair(el, "src-xy", 0, out sx, out sy);
            int sw, sh; ParseOptIntPair(el, "src-wh", 0, out sw, out sh);

            return new ImageElement
            {
                Kind       = ElementKind.Image,
                Id         = RequireAttr(el, "id"),
                Name       = OptAttr(el, "name"),
                TextureRef = RequireAttr(el, "texture-id"),
                X          = x,
                Y          = y,
                Width      = w,
                Height     = h,
                Filter     = filter,
                TexX       = sx,
                TexY       = sy,
                TexW       = sw,
                TexH       = sh
            };
        }

        private static LabelElement ParseLabel(XElement el)
        {
            string wrapInput = OptAttr(el, "wrap") ?? "nowrap";
            string wrap;
            if (!WrapMap.TryGetValue(wrapInput, out wrap))
                throw new ParseException(At(el) + "wrap '" + wrapInput + "' must be one of: " + Join(WrapMap.Keys) + ".");

            int lx, ly; ParseIntPair(el, "xy", out lx, out ly);
            int lw, lh; ParseOptIntPair(el, "wh", 0, out lw, out lh);

            return new LabelElement
            {
                Kind    = ElementKind.Label,
                Id      = RequireAttr(el, "id"),
                Name    = OptAttr(el, "name"),
                X       = lx,
                Y       = ly,
                Width   = lw,
                Height  = lh,
                Size    = ParseInt(el, "size"),
                Color   = ParseColor(el, "color"),
                FontRef = RequireAttr(el, "font-id"),
                Content = OptAttr(el, "content") ?? "",
                Wrap    = wrap
            };
        }

        private static BreadcrumbElement ParseBreadcrumb(XElement el)
        {
            int bx, by; ParseIntPair(el, "xy", out bx, out by);
            int bw, bh; ParseIntPair(el, "wh", out bw, out bh);

            var b = new BreadcrumbElement
            {
                Kind         = ElementKind.Breadcrumb,
                Id           = RequireAttr(el, "id"),
                Name         = OptAttr(el, "name"),
                X            = bx,
                Y            = by,
                Width        = bw,
                Height       = bh,
                FontRef         = RequireAttr(el, "font-id"),
                FontSize        = ParseAutoOrInt(el, "font-size", 16),
                BorderRadius    = ParseAutoOrInt(el, "corner-radius", 6),
                BorderThickness = ParseAutoOrInt(el, "border-thickness", 0),
                BgColor         = OptAttr(el, "bg-color"),
                BorderColor     = OptAttr(el, "border-color"),
                TextColor       = OptAttr(el, "text-color"),
                ChevronColor    = OptAttr(el, "chevron-color")
            };

            if (b.BgColor != null) { ValidateColor(el, "bg-color", b.BgColor); b.BgColor = NormalizeColor(b.BgColor); }
            if (b.BorderColor != null) { ValidateColor(el, "border-color", b.BorderColor); b.BorderColor = NormalizeColor(b.BorderColor); }
            if (b.TextColor != null) { ValidateColor(el, "text-color", b.TextColor); b.TextColor = NormalizeColor(b.TextColor); }
            if (b.ChevronColor != null) { ValidateColor(el, "chevron-color", b.ChevronColor); b.ChevronColor = NormalizeColor(b.ChevronColor); }

            foreach (var child in el.Elements("segment"))
            {
                b.Segments.Add(new BreadcrumbSegment { Text = RequireAttr(child, "text") });
            }

            return b;
        }

        // ---- attribute helpers ----

        private static string RequireAttr(XElement el, string name)
        {
            var a = el.Attribute(name);
            if (a == null)
                throw new ParseException(At(el) + "<" + el.Name.LocalName + "> is missing required attribute '" + name + "'.");
            return a.Value;
        }

        private static string OptAttr(XElement el, string name)
        {
            var a = el.Attribute(name);
            return a == null ? null : a.Value;
        }

        private static int ParseInt(XElement el, string name)
        {
            string raw = RequireAttr(el, name);
            int v;
            if (!int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out v))
                throw new ParseException(At(el) + "attribute '" + name + "' must be an integer (got '" + raw + "').");
            return v;
        }

        private static int ParseAutoOrInt(XElement el, string name, int autoValue)
        {
            string raw = OptAttr(el, name);
            if (raw == null) return autoValue;
            if (raw == "auto") return autoValue;
            int v;
            if (!int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out v))
                throw new ParseException(At(el) + "attribute '" + name + "' must be 'auto' or an integer (got '" + raw + "').");
            return v;
        }

        private static float ParseFloat(XElement el, string name)
        {
            string raw = RequireAttr(el, name);
            float v;
            if (!float.TryParse(raw, NumberStyles.Float, CultureInfo.InvariantCulture, out v))
                throw new ParseException(At(el) + "attribute '" + name + "' must be a number (got '" + raw + "').");
            return v;
        }

        private static void ParseIntPair(XElement el, string name, out int a, out int b)
        {
            string raw = RequireAttr(el, name);
            var parts = raw.Split(',');
            if (parts.Length != 2)
                throw new ParseException(At(el) + "attribute '" + name + "' must be 'X,Y' (got '" + raw + "').");

            if (!int.TryParse(parts[0].Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out a) ||
                !int.TryParse(parts[1].Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out b))
                throw new ParseException(At(el) + "attribute '" + name + "' must be two integers separated by ',' (got '" + raw + "').");
        }

        private static void ParseOptIntPair(XElement el, string name, int defaultValue, out int a, out int b)
        {
            string raw = OptAttr(el, name);
            if (raw == null || raw == "auto") { a = defaultValue; b = defaultValue; return; }
            var parts = raw.Split(',');
            if (parts.Length != 2)
                throw new ParseException(At(el) + "attribute '" + name + "' must be 'X,Y' or 'auto' (got '" + raw + "').");

            if (!int.TryParse(parts[0].Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out a) ||
                !int.TryParse(parts[1].Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out b))
                throw new ParseException(At(el) + "attribute '" + name + "' must be two integers separated by ',' (got '" + raw + "').");
        }

        private static void ParseFloatPair(XElement el, string name, out float a, out float b)
        {
            string raw = RequireAttr(el, name);
            var parts = raw.Split(',');
            if (parts.Length != 2)
                throw new ParseException(At(el) + "attribute '" + name + "' must be 'X,Y' (got '" + raw + "').");

            if (!float.TryParse(parts[0].Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out a) ||
                !float.TryParse(parts[1].Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out b))
                throw new ParseException(At(el) + "attribute '" + name + "' must be two numbers separated by ',' (got '" + raw + "').");
        }

        private static string ParseColor(XElement el, string name)
        {
            string raw = RequireAttr(el, name);
            ValidateColor(el, name, raw);
            return NormalizeColor(raw);
        }

        private static void ValidateColor(XElement el, string name, string raw)
        {
            if (HexColor.IsMatch(raw)) return;
            if (ConstColor.IsMatch(raw)) return;
            throw new ParseException(At(el) + "attribute '" + name + "' must be a COLOR_* constant or 0xAARRGGBB hex (got '" + raw + "').");
        }

        // expand 0xRRGGBB to 0xFFRRGGBB (full alpha)
        private static string NormalizeColor(string raw)
        {
            if (raw.StartsWith("0x", StringComparison.OrdinalIgnoreCase) && raw.Length == 8)
                return "0xFF" + raw.Substring(2);
            return raw;
        }

        private static string At(XElement el)
        {
            var li = (IXmlLineInfo)el;
            return li.HasLineInfo()
                ? "line " + li.LineNumber + ": "
                : "";
        }

        private static string Join(IEnumerable<string> set)
        {
            return string.Join(", ", set.OrderBy(s => s, StringComparer.Ordinal).ToArray());
        }
    }
}
