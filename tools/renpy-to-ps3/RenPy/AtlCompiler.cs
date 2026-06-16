using System.Collections;
using System.Collections.Generic;
using System.Globalization;

namespace RenpyToPs3.RenPy
{
    // ATL (Animation & Transformation Language) compiler. Parses a renpy.atl.RawBlock (the `atl`
    // attribute of a Show/Image statement) into a flat keyframe program the PS3 player can replay.
    // Faithful translation of the common ATL subset: timed property interpolation + pause + repeat.
    // Field names match renpy/atl.py (RawBlock.statements; RawMultipurpose.warper/duration/properties/
    // expressions; RawRepeat.repeats). Values must be constants (literals); non-constant property
    // expressions and advanced statements (parallel/choice/on/time/function/splines) are skipped + noted.

    public enum AtlWarper : byte { Instant = 0, Linear = 1, Pause = 2, Ease = 3, EaseIn = 4, EaseOut = 5 }

    // Numeric transform properties. MUST stay in lockstep with the C `AtlProp` enum (atl.h).
    public enum AtlProp : byte { Xpos = 0, Ypos, Xanchor, Yanchor, Xalign, Yalign, Zoom, Xzoom, Yzoom, Alpha, Rotate }

    public sealed class AtlKey
    {
        public AtlWarper Warper;
        public int DurMs;
        public readonly List<KeyValuePair<AtlProp, int>> Props = new List<KeyValuePair<AtlProp, int>>();   // value*1000 (milli)
    }

    public sealed class AtlProgram
    {
        public readonly List<AtlKey> Keys = new List<AtlKey>();
        public int RepeatCount;   // 0 = play once; -1 = loop forever; N = repeat N times

        public string Key()
        {
            System.Text.StringBuilder sb = new System.Text.StringBuilder();
            sb.Append('R').Append(RepeatCount).Append(';');
            foreach (AtlKey k in Keys)
            {
                sb.Append((int)k.Warper).Append('@').Append(k.DurMs).Append(':');
                foreach (KeyValuePair<AtlProp, int> pr in k.Props) sb.Append((int)pr.Key).Append('=').Append(pr.Value).Append(',');
                sb.Append(';');
            }
            return sb.ToString();
        }
    }

    public static class AtlCompiler
    {
        public static AtlProgram Compile(PyObject block, List<string> notes)
        {
            if (block == null) return null;
            IDictionary st = StateDict(block);
            if (st == null) return null;
            IList stmts = Get(st, "statements") as IList;
            if (stmts == null) return null;

            AtlProgram prog = new AtlProgram();
            foreach (object so in stmts)
            {
                PyObject s = so as PyObject;
                if (s == null) continue;
                string cls = s.ClassName ?? "";

                if (cls.EndsWith(".RawMultipurpose", System.StringComparison.Ordinal))
                {
                    IDictionary ms = StateDict(s);
                    if (ms == null) continue;
                    IList exprs = Get(ms, "expressions") as IList;
                    IList props = Get(ms, "properties") as IList;

                    AtlKey key = new AtlKey();
                    key.Warper = MapWarper(AsStr(Get(ms, "warper")));
                    double dur;
                    TryNum(AstNode.AsText(Get(ms, "duration")), out dur);
                    key.DurMs = (int)(dur * 1000.0 + 0.5);

                    if (props != null)
                        foreach (object po in props)
                        {
                            object a, b;
                            if (!Pair(po, out a, out b)) continue;
                            string pname = a as string; if (pname == null) pname = AstNode.AsText(a);
                            string pval = AstNode.AsText(b); if (pval == null) pval = b as string;
                            AddProp(key, pname, pval, notes);
                        }

                    // The displayable/transition form (`expressions`, e.g. show X "foo.png" with Dissolve)
                    // can't be represented as a property keyframe; skip those (unless it's a timed hold).
                    if ((exprs != null && exprs.Count > 0) && key.Props.Count == 0 && key.Warper != AtlWarper.Pause)
                    {
                        if (notes != null) notes.Add("ATL displayable/contains form not animated");
                        continue;
                    }
                    if (key.Props.Count > 0 || key.DurMs > 0 || key.Warper == AtlWarper.Pause)
                        prog.Keys.Add(key);
                }
                else if (cls.EndsWith(".RawRepeat", System.StringComparison.Ordinal))
                {
                    IDictionary rs = StateDict(s);
                    string rep = rs != null ? AstNode.AsText(Get(rs, "repeats")) : null;
                    int n;
                    prog.RepeatCount = (rep != null && int.TryParse(rep.Trim(), out n)) ? n : -1;   // no count -> loop forever
                }
                else if (notes != null) notes.Add("ATL statement unsupported: " + cls);
            }
            return prog.Keys.Count > 0 ? prog : null;
        }

        private static void AddProp(AtlKey key, string name, string val, List<string> notes)
        {
            if (name == null) return;
            name = name.Trim();
            double v;
            if (!TryNum(val, out v))
            {
                if (notes != null) notes.Add("ATL non-constant property: " + name + " = " + (val ?? "?"));
                return;
            }
            int milli = (int)(v * 1000.0 + (v < 0 ? -0.5 : 0.5));
            switch (name)
            {
                case "xpos":    key.Props.Add(P(AtlProp.Xpos, milli)); break;
                case "ypos":    key.Props.Add(P(AtlProp.Ypos, milli)); break;
                case "xanchor": key.Props.Add(P(AtlProp.Xanchor, milli)); break;
                case "yanchor": key.Props.Add(P(AtlProp.Yanchor, milli)); break;
                case "xalign":  key.Props.Add(P(AtlProp.Xalign, milli)); break;
                case "yalign":  key.Props.Add(P(AtlProp.Yalign, milli)); break;
                case "zoom":    key.Props.Add(P(AtlProp.Zoom, milli)); break;
                case "xzoom":   key.Props.Add(P(AtlProp.Xzoom, milli)); break;
                case "yzoom":   key.Props.Add(P(AtlProp.Yzoom, milli)); break;
                case "alpha":   key.Props.Add(P(AtlProp.Alpha, milli)); break;
                case "rotate":  key.Props.Add(P(AtlProp.Rotate, milli)); break;
                // align/pos/anchor take a (x,y) tuple value -> not a single scalar; skip + note.
                default: if (notes != null) notes.Add("ATL property unsupported: " + name); break;
            }
        }

        private static KeyValuePair<AtlProp, int> P(AtlProp p, int v) { return new KeyValuePair<AtlProp, int>(p, v); }

        private static AtlWarper MapWarper(string w)
        {
            if (w == null) return AtlWarper.Instant;
            switch (w)
            {
                case "linear":  return AtlWarper.Linear;
                case "pause":   return AtlWarper.Pause;
                case "ease":    return AtlWarper.Ease;
                case "easein":  return AtlWarper.EaseIn;
                case "easeout": return AtlWarper.EaseOut;
                default:        return AtlWarper.Linear;   // unknown named warper -> linear (closest faithful default)
            }
        }

        // ---- helpers ----
        private static IDictionary StateDict(PyObject p)
        {
            object[] t = p.State as object[];
            if (t != null && t.Length >= 2 && t[1] is IDictionary) return (IDictionary)t[1];
            return p.State as IDictionary;
        }

        private static object Get(IDictionary d, string key) { return (d != null && d.Contains(key)) ? d[key] : null; }

        private static string AsStr(object o)
        {
            string s = o as string;
            if (s != null) return s;
            return AstNode.AsText(o);
        }

        private static bool Pair(object o, out object a, out object b)
        {
            a = null; b = null;
            object[] t = o as object[];
            if (t != null && t.Length >= 2) { a = t[0]; b = t[1]; return true; }
            IList l = o as IList;
            if (l != null && l.Count >= 2) { a = l[0]; b = l[1]; return true; }
            return false;
        }

        private static bool TryNum(string s, out double v)
        {
            v = 0;
            if (s == null) return false;
            return double.TryParse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out v);
        }
    }
}
