using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace XmlToC
{
    // Emits a .c / .h pair for a ScreenModel that matches the simple-ps3-lib idioms
    // seen in app-sample (palette.c, file-manager.c, etc).

    internal static class CodeGenerator
    {
        public static void Emit(ScreenModel m, string cPath, string hPath)
        {
            File.WriteAllText(hPath, BuildHeader(m), Encoding.UTF8);
            File.WriteAllText(cPath, BuildSource(m, hPath), Encoding.UTF8);
        }

        private static string BuildHeader(ScreenModel m)
        {
            var sb = new StringBuilder();
            sb.Append("#pragma once\n\n");
            sb.Append("#include \"screen.h\"\n\n");
            sb.Append("extern Screen ").Append(m.Name).Append("Screen;\n");
            return sb.ToString();
        }

        private static string BuildSource(ScreenModel m, string hPath)
        {
            // derive the #include path from the header output path
            // uses last two path segments (e.g. "screens/home.h") if available
            string hFile = Path.GetFileName(hPath);
            string hDir = Path.GetFileName(Path.GetDirectoryName(hPath));
            string includePath = !string.IsNullOrEmpty(hDir) ? hDir + "/" + hFile : hFile;

            bool needsFont = m.Fonts.Count > 0
                             || m.Drawables.OfType<LabelElement>().Any()
                             || m.Drawables.OfType<BreadcrumbElement>().Any();
            bool needsTex  = m.Textures.Count > 0;
            bool needsRect = m.Drawables.OfType<RectangleElement>().Any();
            bool needsCircle = m.Drawables.OfType<CircleElement>().Any();
            bool needsTri = m.Drawables.OfType<TriangleElement>().Any();
            bool needsLine = m.Drawables.OfType<LineElement>().Any();
            bool needsLabel = m.Drawables.OfType<LabelElement>().Any();
            bool needsImage = m.Drawables.OfType<ImageElement>().Any();
            bool needsBreadcrumb = m.Drawables.OfType<BreadcrumbElement>().Any();

            var sb = new StringBuilder();
            sb.Append("#include \"").Append(includePath).Append("\"\n");
            sb.Append("#include \"gfx.h\"\n");
            sb.Append("#include \"colors.h\"\n");
            if (needsFont) sb.Append("#include \"font.h\"\n");
            if (needsRect) sb.Append("#include \"ui/rectangle.h\"\n");
            if (needsCircle) sb.Append("#include \"ui/circle.h\"\n");
            if (needsTri) sb.Append("#include \"ui/triangle.h\"\n");
            if (needsLine) sb.Append("#include \"ui/line.h\"\n");
            if (needsImage) sb.Append("#include \"ui/image.h\"\n");
            if (needsLabel) sb.Append("#include \"ui/label.h\"\n");
            if (needsBreadcrumb) sb.Append("#include \"ui/breadcrumb.h\"\n");
            sb.Append('\n');

            // statics: fonts, then textures, then ui components
            foreach (var f in m.Fonts)
                sb.Append("static Font ").Append(f.Id).Append(";\n");
            foreach (var t in m.Textures)
                sb.Append("static GfxTexture ").Append(t.Id).Append(";\n");
            foreach (var r in m.Drawables.OfType<RectangleElement>())
                sb.Append("static Rectangle ").Append(r.Id).Append(";\n");
            foreach (var c in m.Drawables.OfType<CircleElement>())
                sb.Append("static Circle ").Append(c.Id).Append(";\n");
            foreach (var t in m.Drawables.OfType<TriangleElement>())
                sb.Append("static Triangle ").Append(t.Id).Append(";\n");
            foreach (var l in m.Drawables.OfType<LineElement>())
                sb.Append("static Line ").Append(l.Id).Append(";\n");
            foreach (var l in m.Drawables.OfType<LabelElement>())
                sb.Append("static Label ").Append(l.Id).Append(";\n");
            foreach (var img in m.Drawables.OfType<ImageElement>())
                sb.Append("static Image ").Append(img.Id).Append(";\n");
            foreach (var b in m.Drawables.OfType<BreadcrumbElement>())
                sb.Append("static Breadcrumb ").Append(b.Id).Append(";\n");
            sb.Append('\n');

            // init
            sb.Append("static void ").Append(m.Name).Append("Init(void)\n{\n");
            bool first = true;
            foreach (var f in m.Fonts)
            {
                EmitComments(sb, f.LeadingComments, "    ", first);
                first = false;
                sb.Append("    ").Append(f.Id).Append(" = fontOpenSystem(").Append(f.SystemType).Append(");\n");
            }
            foreach (var t in m.Textures)
            {
                EmitComments(sb, t.LeadingComments, "    ", first);
                first = false;
                sb.Append("    ").Append(t.Id).Append(" = gfxLoadTexture(").Append(EscapeC(t.Path)).Append(");\n");
            }
            foreach (var el in m.Drawables)
            {
                EmitComments(sb, el.LeadingComments, "    ", first);
                first = false;
                EmitInit(sb, el);
            }
            sb.Append("}\n\n");

            // resume / update (empty stubs -- this generator targets static visuals)
            sb.Append("static void ").Append(m.Name).Append("Resume(void) {}\n\n");
            sb.Append("static void ").Append(m.Name).Append("Update(void) {}\n\n");

            // draw
            sb.Append("static void ").Append(m.Name).Append("Draw(void)\n{\n");
            bool firstDraw = true;
            if (!string.IsNullOrEmpty(m.Fill))
            {
                sb.Append("    gfxClear(").Append(m.Fill).Append(");\n");
                firstDraw = false;
            }
            foreach (var el in m.Drawables)
            {
                EmitComments(sb, el.LeadingComments, "    ", firstDraw);
                firstDraw = false;
                EmitDraw(sb, el);
            }
            sb.Append("}\n\n");

            // suspend
            sb.Append("static void ").Append(m.Name).Append("Suspend(void) {}\n\n");

            // term
            sb.Append("static void ").Append(m.Name).Append("Term(void)\n{\n");
            foreach (var b in m.Drawables.OfType<BreadcrumbElement>())
                sb.Append("    breadcrumbTerm(&").Append(b.Id).Append(");\n");
            foreach (var f in m.Fonts)
                sb.Append("    fontClose(&").Append(f.Id).Append(");\n");
            sb.Append("}\n\n");

            // screen struct
            sb.Append("Screen ").Append(m.Name).Append("Screen = { ")
              .Append(m.Name).Append("Init, ")
              .Append(m.Name).Append("Resume, ")
              .Append(m.Name).Append("Update, ")
              .Append(m.Name).Append("Draw, ")
              .Append(m.Name).Append("Suspend, ")
              .Append(m.Name).Append("Term, SCREEN_TERMINATED };\n");

            return sb.ToString();
        }

        // ---- init emitters ----

        private static void EmitInit(StringBuilder sb, Element el)
        {
            var rect = el as RectangleElement;
            if (rect != null)
            {
                sb.Append("    rectangleInit(&").Append(rect.Id).Append(", ")
                  .Append(I(rect.X)).Append(", ").Append(I(rect.Y)).Append(", ")
                  .Append(I(rect.Width)).Append(", ").Append(I(rect.Height)).Append(", ")
                  .Append(I(rect.Radius)).Append(", ")
                  .Append(rect.Fill).Append(", ")
                  .Append(I(rect.BorderThickness)).Append(", ")
                  .Append(rect.BorderThickness > 0 && rect.BorderColor != null ? rect.BorderColor : "COLOR_TRANSPARENT").Append(");\n");
                return;
            }

            var circle = el as CircleElement;
            if (circle != null)
            {
                sb.Append("    circleInit(&").Append(circle.Id).Append(", ")
                  .Append(I(circle.Cx)).Append(", ").Append(I(circle.Cy)).Append(", ")
                  .Append(I(circle.Radius)).Append(", ")
                  .Append(circle.Fill).Append(", ")
                  .Append(I(circle.BorderThickness)).Append(", ")
                  .Append(circle.BorderThickness > 0 && circle.BorderColor != null ? circle.BorderColor : "COLOR_TRANSPARENT").Append(");\n");
                return;
            }

            var tri = el as TriangleElement;
            if (tri != null)
            {
                sb.Append("    triangleInit(&").Append(tri.Id).Append(", ")
                  .Append(F(tri.X0)).Append(", ").Append(F(tri.Y0)).Append(", ")
                  .Append(F(tri.X1)).Append(", ").Append(F(tri.Y1)).Append(", ")
                  .Append(F(tri.X2)).Append(", ").Append(F(tri.Y2)).Append(", ")
                  .Append(tri.Fill).Append(", ")
                  .Append(I(tri.BorderThickness)).Append(", ")
                  .Append(tri.BorderThickness > 0 && tri.BorderColor != null ? tri.BorderColor : "COLOR_TRANSPARENT").Append(");\n");
                return;
            }

            var line = el as LineElement;
            if (line != null)
            {
                sb.Append("    lineInit(&").Append(line.Id).Append(", ")
                  .Append(I(line.X0)).Append(", ").Append(I(line.Y0)).Append(", ")
                  .Append(I(line.X1)).Append(", ").Append(I(line.Y1)).Append(", ")
                  .Append(I(line.Thickness)).Append(", ")
                  .Append(line.Color).Append(");\n");
                return;
            }

            var label = el as LabelElement;
            if (label != null)
            {
                sb.Append("    labelInit(&").Append(label.Id).Append(", &")
                  .Append(label.FontRef).Append(", ")
                  .Append(I(label.X)).Append(", ").Append(I(label.Y)).Append(", ")
                  .Append(EmitAuto(label.Width)).Append(", ")
                  .Append(EmitAuto(label.Height)).Append(", ")
                  .Append(I(label.Size)).Append(", ")
                  .Append(label.Color).Append(", ")
                  .Append(label.Wrap).Append(");\n");
                if (!string.IsNullOrEmpty(label.Content))
                    sb.Append("    labelSetText(&").Append(label.Id).Append(", ").Append(EscapeC(label.Content)).Append(");\n");
                return;
            }

            var img = el as ImageElement;
            if (img != null)
            {
                sb.Append("    imageInit(&").Append(img.Id).Append(", ")
                  .Append(img.TextureRef).Append(", ")
                  .Append(I(img.X)).Append(", ").Append(I(img.Y)).Append(", ")
                  .Append(EmitAuto(img.Width)).Append(", ")
                  .Append(EmitAuto(img.Height)).Append(", ")
                  .Append(EmitAuto(img.TexX)).Append(", ").Append(EmitAuto(img.TexY)).Append(", ")
                  .Append(EmitAuto(img.TexW)).Append(", ").Append(EmitAuto(img.TexH)).Append(", ")
                  .Append(img.Filter).Append(");\n");
                return;
            }

            var bread = el as BreadcrumbElement;
            if (bread != null)
            {
                sb.Append("    breadcrumbInit(&").Append(bread.Id).Append(", &")
                  .Append(bread.FontRef).Append(", ")
                  .Append(I(bread.X)).Append(", ").Append(I(bread.Y)).Append(", ")
                  .Append(I(bread.Width)).Append(", ").Append(I(bread.Height)).Append(", ")
                  .Append(bread.BgColor ?? "COLOR_SLATE_800").Append(", ")
                  .Append(bread.BorderThickness > 0 && bread.BorderColor != null ? bread.BorderColor : "COLOR_TRANSPARENT").Append(", ")
                  .Append(bread.TextColor ?? "COLOR_WHITE").Append(", ")
                  .Append(bread.ChevronColor ?? "COLOR_SLATE_400").Append(", ")
                  .Append(I(bread.BorderRadius)).Append(", ")
                  .Append(I(bread.BorderThickness)).Append(", ")
                  .Append(I(bread.FontSize)).Append(");\n");
                foreach (var seg in bread.Segments)
                    sb.Append("    breadcrumbPush(&").Append(bread.Id).Append(", ").Append(EscapeC(seg.Text)).Append(");\n");
                return;
            }
        }

        // ---- draw emitters ----

        private static void EmitDraw(StringBuilder sb, Element el)
        {
            var rect   = el as RectangleElement; if (rect   != null) { sb.Append("    rectangleDraw(&").Append(rect.Id);   EndCall(sb, Comment(rect));   return; }
            var circle = el as CircleElement;    if (circle != null) { sb.Append("    circleDraw(&").Append(circle.Id);    EndCall(sb, Comment(circle)); return; }
            var tri    = el as TriangleElement;  if (tri    != null) { sb.Append("    triangleDraw(&").Append(tri.Id);     EndCall(sb, Comment(tri));    return; }
            var line   = el as LineElement;      if (line   != null) { sb.Append("    lineDraw(&").Append(line.Id);        EndCall(sb, Comment(line));   return; }
            var image  = el as ImageElement;     if (image  != null) { sb.Append("    imageDraw(&").Append(image.Id);      EndCall(sb, Comment(image));  return; }
            var label  = el as LabelElement;     if (label  != null) { sb.Append("    labelDraw(&").Append(label.Id);      EndCall(sb, Comment(label));  return; }
            var bread  = el as BreadcrumbElement; if (bread != null) { sb.Append("    breadcrumbDraw(&").Append(bread.Id); EndCall(sb, Comment(bread));  return; }
        }

        // returns the display name for trailing comments (only if name is set)
        private static string Comment(Element el)
        {
            return !string.IsNullOrEmpty(el.Name) ? el.Name : null;
        }

        // ---- shared helpers ----

        private static void EndCall(StringBuilder sb, string trail)
        {
            sb.Append(");");
            if (!string.IsNullOrEmpty(trail))
                sb.Append("  // ").Append(trail);
            sb.Append('\n');
        }

        // ---- emit helpers ----

        // emits each XML comment block as one or more // line comments, indented
        // to match the surrounding code. Multi-line comment bodies are split on
        // newlines; whitespace-only lines are dropped. A blank line is emitted
        // before the comment block for visual separation.
        private static void EmitComments(StringBuilder sb, List<string> comments, string indent, bool isFirst)
        {
            if (comments == null || comments.Count == 0) return;
            if (!isFirst) sb.Append('\n');
            foreach (var c in comments)
            {
                string normalized = c.Replace("\r\n", "\n").Replace('\r', '\n');
                foreach (var line in normalized.Split('\n'))
                {
                    string trimmed = line.Trim();
                    if (trimmed.Length == 0) continue;
                    sb.Append(indent).Append("// ").Append(trimmed).Append('\n');
                }
            }
        }

        private static string I(int v)
        {
            return v.ToString(CultureInfo.InvariantCulture);
        }

        private static string F(float v)
        {
            int ivalue = (int)v;
            if (v == ivalue)
                return ivalue.ToString(CultureInfo.InvariantCulture);
            return v.ToString("0.0###", CultureInfo.InvariantCulture) + "f";
        }

        private static string EmitAuto(int v)
        {
            return v == 0 ? "AUTO" : I(v);
        }

        private static string EscapeC(string s)
        {
            var sb = new StringBuilder();
            sb.Append('"');
            foreach (char c in s)
            {
                switch (c)
                {
                    case '\\': sb.Append("\\\\"); break;
                    case '"':  sb.Append("\\\""); break;
                    case '\n': sb.Append("\\n");  break;
                    case '\r': sb.Append("\\r");  break;
                    case '\t': sb.Append("\\t");  break;
                    default:
                        if (c < 0x20)
                            sb.Append("\\x").Append(((int)c).ToString("x2", CultureInfo.InvariantCulture));
                        else
                            sb.Append(c);
                        break;
                }
            }
            sb.Append('"');
            return sb.ToString();
        }
    }
}
