using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace RenpyToPs3.RenPy
{
    // Overlay (HUD) extractor. Old Ren'Py games draw an in-game HUD with Python functions registered in
    // config.overlay_functions, composed via the immediate-mode ui.* API. We can't run that Python, so
    // we parse each overlay function body into a flat list of GUARDED widgets the player can replay:
    //   ui.image("file" [,xpos,ypos])                         -> Image
    //   ui.text(template, xpos, ypos [,...])                  -> Text
    //   ui.imagebutton("idle","hover", clicked=ACTION [,...]) -> ImageButton  (pos from a preceding ui.vbox)
    // The enclosing if/elif conditions become each widget's guard (compiled to an RPN expr; AND of
    // ancestors). ACTION: ccinc("L")/ui.callsinnewcontext("L") -> call:L; ui.gamemenus("P") -> menu:P;
    // anything else (toggle_*, etc.) -> inert. Loops, str()/arithmetic text, and else-branch negation are
    // first-slice limitations (flagged), not faithful yet.

    public enum OvKind : byte { Image = 0, Text = 1, ImageButton = 2 }

    public sealed class OvWidget
    {
        public OvKind Kind;
        public int    X, Y;
        public string A = "";       // Image: file; Text: template; ImageButton: idle file
        public string B = "";       // ImageButton: hover file
        public string Action = "";  // ImageButton: "call:<label>" / "menu:<prompt>" / "" (inert)
        public int    GuardExpr = -1;   // RPN expr id gating this widget (-1 = always)
    }

    public sealed class OverlayDef
    {
        public string Name = "";
        public readonly List<OvWidget> Widgets = new List<OvWidget>();
        public string Key()
        {
            StringBuilder sb = new StringBuilder(Name).Append('|');
            foreach (OvWidget w in Widgets)
                sb.Append((int)w.Kind).Append(':').Append(w.X).Append(',').Append(w.Y).Append(',')
                  .Append(w.A).Append(',').Append(w.B).Append(',').Append(w.Action).Append(',').Append(w.GuardExpr).Append(';');
            return sb.ToString();
        }
    }

    public static class OverlayCompiler
    {
        private static readonly Regex DefRe   = new Regex(@"^(\s*)def\s+([A-Za-z_]\w*)\s*\(\s*\)\s*:");
        private static readonly Regex IfRe    = new Regex(@"^(\s*)(if|elif)\s+(.+?)\s*:\s*$");
        private static readonly Regex ElseRe  = new Regex(@"^(\s*)else\s*:\s*$");
        private static readonly Regex VboxRe  = new Regex(@"ui\.(?:vbox|fixed)\(([^)]*)\)");
        private static readonly Regex CloseRe = new Regex(@"ui\.close\(\)");
        private static readonly Regex ImageRe = new Regex(@"ui\.image\(\s*[uU]?[""']([^""']+)[""']([^)]*)\)");
        private static readonly Regex TextRe  = new Regex(@"ui\.text\((.*)\)\s*$");
        private static readonly Regex BtnRe   = new Regex(@"ui\.imagebutton\(\s*[uU]?[""']([^""']+)[""']\s*,\s*[uU]?[""']([^""']+)[""']([^)]*\bclicked\s*=\s*[^,)]+)");
        private static readonly Regex XposRe  = new Regex(@"xpos\s*=\s*(-?\d+)");
        private static readonly Regex YposRe  = new Regex(@"ypos\s*=\s*(-?\d+)");
        private static readonly Regex ClickRe = new Regex(@"clicked\s*=\s*([A-Za-z_][\w.]*)\(\s*[uU]?[""']([^""']+)[""']\s*\)");

        // Scan a whole python source for overlay function defs; append any found to `into`.
        public static void ParseAll(string src, IrProgram ir, Dictionary<string, OverlayDef> into)
        {
            if (string.IsNullOrEmpty(src) || src.IndexOf("ui.", System.StringComparison.Ordinal) < 0) return;
            string[] lines = src.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
            for (int i = 0; i < lines.Length; i++)
            {
                Match d = DefRe.Match(lines[i]);
                if (!d.Success) continue;
                int defIndent = d.Groups[1].Value.Length;
                // collect the body (lines indented deeper than the def, until dedent)
                int j = i + 1;
                List<string> body = new List<string>();
                for (; j < lines.Length; j++)
                {
                    if (lines[j].Trim().Length == 0) { body.Add(lines[j]); continue; }
                    if (Indent(lines[j]) <= defIndent) break;
                    body.Add(lines[j]);
                }
                OverlayDef ov = ParseBody(d.Groups[2].Value, body, ir);
                if (ov != null && ov.Widgets.Count > 0) into[ov.Name] = ov;
                i = j - 1;
            }
        }

        private static int Indent(string s) { int n = 0; while (n < s.Length && (s[n] == ' ' || s[n] == '\t')) n++; return n; }

        private static OverlayDef ParseBody(string name, List<string> body, IrProgram ir)
        {
            OverlayDef ov = new OverlayDef(); ov.Name = name;
            // guard stack: (indent, effectiveConditionSource). Active guard = AND of all on the stack.
            List<KeyValuePair<int, string>> guards = new List<KeyValuePair<int, string>>();
            // Per-indent record of the if/elif conditions already seen in the CURRENT chain, so an
            // `elif`/`else` can negate its earlier siblings (faithful exclusivity). A non-chain
            // statement at an indent, or a fresh `if`, resets that indent's chain.
            Dictionary<int, List<string>> chain = new Dictionary<int, List<string>>();
            int pendX = 0, pendY = 0, pendHave = 0;   // position from the last ui.vbox until ui.close()

            foreach (string raw in body)
            {
                string line = raw.Trim();
                if (line.Length == 0) continue;
                int ind = Indent(raw);
                // pop guards that we've dedented out of
                while (guards.Count > 0 && guards[guards.Count - 1].Key >= ind) guards.RemoveAt(guards.Count - 1);

                Match ifm = IfRe.Match(raw);
                if (ifm.Success)
                {
                    string kw = ifm.Groups[2].Value, cond = ifm.Groups[3].Value;
                    List<string> prior;
                    string eff;
                    if (kw == "if") { prior = new List<string>(); chain[ind] = prior; eff = "(" + cond + ")"; }
                    else            { if (!chain.TryGetValue(ind, out prior)) { prior = new List<string>(); chain[ind] = prior; } eff = NegateAnd(prior, cond); }   // elif: not(earlier) and this
                    guards.Add(new KeyValuePair<int, string>(ind, eff));
                    chain[ind].Add(cond);   // this branch's own condition negates later siblings
                    continue;
                }
                if (ElseRe.Match(raw).Success)
                {
                    List<string> prior; if (!chain.TryGetValue(ind, out prior)) prior = new List<string>();
                    guards.Add(new KeyValuePair<int, string>(ind, NegateAnd(prior, null)));   // else: not(any earlier)
                    chain.Remove(ind);   // the chain ends at else
                    continue;
                }
                chain.Remove(ind);   // a non if/elif/else statement here ends any chain at this indent

                if (VboxRe.IsMatch(line)) { Match v = VboxRe.Match(line); ReadPos(v.Groups[1].Value, ref pendX, ref pendY, ref pendHave); continue; }
                if (CloseRe.IsMatch(line)) { pendHave = 0; continue; }

                int guard = CompileGuard(guards, ir);

                Match b = BtnRe.Match(line);
                if (b.Success)
                {
                    OvWidget w = new OvWidget { Kind = OvKind.ImageButton, A = b.Groups[1].Value, B = b.Groups[2].Value, GuardExpr = guard };
                    if (pendHave != 0) { w.X = pendX; w.Y = pendY; }
                    PosFrom(b.Groups[3].Value, w);
                    w.Action = ParseAction(line);
                    ov.Widgets.Add(w);
                    continue;
                }
                Match im = ImageRe.Match(line);
                if (im.Success)
                {
                    OvWidget w = new OvWidget { Kind = OvKind.Image, A = im.Groups[1].Value, GuardExpr = guard };
                    if (pendHave != 0) { w.X = pendX; w.Y = pendY; }
                    PosFrom(im.Groups[2].Value, w);
                    ov.Widgets.Add(w);
                    continue;
                }
                Match tx = TextRe.Match(line);
                if (tx.Success)
                {
                    OvWidget w = new OvWidget { Kind = OvKind.Text, A = tx.Groups[1].Value.Trim(), GuardExpr = guard };
                    if (pendHave != 0) { w.X = pendX; w.Y = pendY; }
                    PosFrom(tx.Groups[1].Value, w);
                    ov.Widgets.Add(w);
                    continue;
                }
            }
            return ov;
        }

        // Build "not (p1) and not (p2) ... and (cond)" for an elif/else: the branch only fires when
        // every earlier sibling in the chain was false. cond == null => an `else` (negations only).
        private static string NegateAnd(List<string> priors, string cond)
        {
            List<string> parts = new List<string>();
            foreach (string p in priors) parts.Add("not (" + p + ")");
            if (cond != null) parts.Add("(" + cond + ")");
            if (parts.Count == 0) return "True";
            return string.Join(" and ", parts.ToArray());
        }

        private static int CompileGuard(List<KeyValuePair<int, string>> guards, IrProgram ir)
        {
            List<string> parts = new List<string>();
            foreach (KeyValuePair<int, string> g in guards) if (g.Value != "True") parts.Add("(" + g.Value + ")");
            if (parts.Count == 0) return -1;
            return ir.CompileExpr(string.Join(" and ", parts.ToArray()));
        }

        private static void ReadPos(string args, ref int x, ref int y, ref int have)
        {
            Match mx = XposRe.Match(args), my = YposRe.Match(args);
            x = mx.Success ? int.Parse(mx.Groups[1].Value) : 0;
            y = my.Success ? int.Parse(my.Groups[1].Value) : 0;
            have = 1;
        }
        private static void PosFrom(string args, OvWidget w)
        {
            Match mx = XposRe.Match(args), my = YposRe.Match(args);
            if (mx.Success) w.X = int.Parse(mx.Groups[1].Value);
            if (my.Success) w.Y = int.Parse(my.Groups[1].Value);
        }

        // clicked=ccinc("status_menu") / ui.callsinnewcontext("X") -> call:X ; ui.gamemenus("P") -> menu:P
        private static string ParseAction(string line)
        {
            Match c = ClickRe.Match(line);
            if (!c.Success) return "";
            string fn = c.Groups[1].Value, arg = c.Groups[2].Value;
            if (fn.EndsWith("gamemenus")) return "menu:" + arg;
            return "call:" + arg;   // ccinc / callsinnewcontext / curried call -> call that label
        }
    }
}
