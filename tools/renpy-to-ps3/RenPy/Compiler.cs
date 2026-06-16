using System.Collections;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace RenpyToPs3.RenPy
{
    // Lowers a Ren'Py AST (top-level statement list) into flat IR. Linear-core nodes are
    // supported; anything else is recorded in IrProgram.Unsupported and emitted as Nop, so
    // conversion fails loudly at compile time rather than crashing on the device.
    public static class Compiler
    {
        public static IrProgram Compile(IList statements)
        {
            List<IList> one = new List<IList>();
            one.Add(statements);
            return CompileUnits(one);
        }

        public static IrProgram CompileUnits(IEnumerable<IList> units) { return CompileUnits(units, false); }

        public static IrProgram CompileUnits(IEnumerable<IList> units, bool normalizeText)
        {
            IrProgram p = new IrProgram();
            foreach (IList u in units)
                foreach (AstNode n in AstNode.ToNodes(u))
                {
                    // Top-level init/python (Ren'Py's init phase: var defaults, config, overlay
                    // registration) runs BEFORE the game; buffer it into the __init__ prologue the
                    // player runs at startup. In-label `$` python is lowered via recursion (stays inline).
                    string s = n.Short;
                    if (s == "Init" || s == "EarlyPython" || s == "Python") { p.BeginInit(); Lower(n, p); p.EndInit(); }
                    else Lower(n, p);
                }
            ResolveCharacters(p);
            if (normalizeText) NormalizeDisplay(p);

            // Append the init prologue under `__init__` (Label + buffered ops + Return) so the player can
            // run it once at startup. These ops are flat (no jumps), so addresses need no fixup.
            if (p.InitCode.Count > 0)
            {
                p.Labels["__init__"] = p.Here;
                p.Emit(new Instr(IrOp.Label, p.Intern("__init__")));
                foreach (Instr ii in p.InitCode) p.Code.Add(ii);
                p.Emit(new Instr(IrOp.Return));
            }

            p.Emit(new Instr(IrOp.End));
            Resolve(p);
            return p;
        }

        // Map common Unicode punctuation to ASCII so the PS3 system font can render text
        // when the game's TTF can't be loaded. Only display strings (Say/Choice) are touched.
        private static void NormalizeDisplay(IrProgram p)
        {
            foreach (Instr ins in p.Code)
            {
                if (ins.Op == IrOp.Say)
                {
                    if (ins.A >= 0) ins.A = p.Intern(Normalize(p.Str(ins.A)));
                    ins.B = p.Intern(Normalize(p.Str(ins.B)));
                }
                else if (ins.Op == IrOp.Choice)
                {
                    ins.A = p.Intern(Normalize(p.Str(ins.A)));
                }
            }
        }

        private static string Normalize(string s)
        {
            if (string.IsNullOrEmpty(s)) return s;
            bool any = false;
            for (int i = 0; i < s.Length; i++) if (s[i] > 127) { any = true; break; }
            if (!any) return s;

            StringBuilder sb = new StringBuilder(s.Length);
            foreach (char c in s)
            {
                switch ((int)c)
                {
                    case 0x2018: case 0x2019: case 0x2032: sb.Append('\''); break;   // ' ' ′
                    case 0x201C: case 0x201D: case 0x2033: sb.Append('"'); break;     // " " ″
                    case 0x2013: case 0x2014: sb.Append('-'); break;                 // – —
                    case 0x2026: sb.Append("..."); break;                            // …
                    case 0x00A0: sb.Append(' '); break;                              // nbsp
                    case 0x2022: sb.Append('*'); break;                              // •
                    default: sb.Append(c); break;
                }
            }
            return sb.ToString();
        }

        private static void Lower(AstNode n, IrProgram p)
        {
            switch (n.Short)
            {
                case "Label":
                    {
                        string name = n.LabelName();
                        if (name == null) name = "@" + p.Here;
                        p.Labels[name] = p.Here;
                        p.Emit(new Instr(IrOp.Label, p.Intern(n.LabelName())));
                        foreach (AstNode c in n.Block("block")) Lower(c, p);
                        break;
                    }

                case "Say":
                    {
                        string who = n.Text("who");
                        string what = n.Text("what");
                        if (what == null) what = "";
                        // Say attributes (`t happy "..."`) imply a `show <tag> <attrs>` in Ren'Py
                        // (renpy/ast.py Say.execute -> predict/show). We don't resolve a
                        // character's image tag yet, so record the fidelity loss instead of
                        // silently dropping it.
                        object attrs = n.Raw("attributes");
                        System.Collections.IList attrList = attrs as System.Collections.IList;
                        if (attrList != null && attrList.Count > 0)
                            p.Notes.Add("say-attributes ignored: " + (who ?? "?") + " (+" + attrList.Count + " attr)");
                        p.Emit(new Instr(IrOp.Say, who != null ? p.Intern(who) : -1, p.Intern(what)));
                        break;
                    }

                case "Scene": p.Emit(new Instr(IrOp.Scene, p.Intern(n.ImspecName()), -1, p.CompileAtl(n))); break;
                case "Show": p.Emit(new Instr(IrOp.Show, p.Intern(n.ImspecName()), p.Intern(n.ImspecAtList()), p.CompileAtl(n))); break;
                case "Hide": p.Emit(new Instr(IrOp.Hide, p.Intern(n.ImspecName()))); break;
                case "With": { string e = n.Text("expr"); p.Emit(new Instr(IrOp.With, p.Intern(e == null ? "" : e))); break; }

                case "Jump": { Instr ins = new Instr(IrOp.Jump); ins.Sym = n.Text("target"); p.Emit(ins); break; }
                case "Call": { Instr ins = new Instr(IrOp.Call); ins.Sym = n.Text("label"); p.Emit(ins); break; }
                case "Return": p.Emit(new Instr(IrOp.Return)); break;
                case "Pass": p.Emit(new Instr(IrOp.Nop)); break;

                case "Python":
                case "EarlyPython":
                case "Init":
                    {
                        string src = n.PyCodeSource();
                        if (src != null)
                        {
                            // Keep PyExec (its source feeds Character() name resolution), and
                            // additionally lower any simple `name = expr` statements to Assign
                            // ops the VM can execute. Non-assignment Python stays a no-op.
                            p.Emit(new Instr(IrOp.PyExec, p.Intern(src)));
                            EmitNativeCalls(src, p);
                            EmitAssignments(src, p);
                        }
                        else foreach (AstNode c in n.Block("block")) Lower(c, p);
                        break;
                    }

                case "Default":
                case "Define":
                    {
                        string varname = n.Text("varname");
                        if (varname == null) { object vn = n.Raw("varname"); varname = vn == null ? "" : vn.ToString(); }
                        string code = CodeOf(n);
                        // C = compiled value expression (-1 if it's a Character()/unsupported RHS;
                        // B keeps the raw source for Character() name resolution).
                        int valExpr = p.CompileExpr(code);
                        p.Emit(new Instr(IrOp.Default, p.Intern(varname), p.Intern(code == null ? "" : code), valExpr));
                        break;
                    }

                case "Image":
                    {
                        string code = CodeOf(n);
                        p.Emit(new Instr(IrOp.Image, p.Intern(n.ImageName()), p.Intern(code == null ? "" : code), p.CompileAtl(n)));
                        break;
                    }

                case "UserStatement": { string line = n.Text("line"); p.Emit(new Instr(IrOp.User, p.Intern(line == null ? "" : line))); break; }

                case "Menu": LowerMenu(n, p); break;
                case "If": LowerIf(n, p); break;
                case "While": LowerWhile(n, p); break;

                default:
                    p.Unsupported.Add(n.Short);
                    p.Emit(new Instr(IrOp.Nop));
                    break;
            }
        }

        private sealed class MenuChoice
        {
            public int Cap;
            public int Cond;
            public List<AstNode> Block;
            public MenuChoice(int cap, int cond, List<AstNode> block) { Cap = cap; Cond = cond; Block = block; }
        }

        private static void LowerMenu(AstNode n, IrProgram p)
        {
            IList items = n.Raw("items") as IList;
            List<MenuChoice> choices = new List<MenuChoice>();
            int captionId = -1;   // a menu CAPTION (an item with NO block) -> shown by the narrator in the dialogue box
            if (items != null)
                foreach (object it in items)
                {
                    object[] t = it as object[];
                    if (t == null || t.Length < 2) continue;
                    string capTxt = AstNode.AsText(t[0]);
                    // Ren'Py menu item = (label, condition, block). The CAPTION has block == None (no
                    // block); it's not selectable -- Ren'Py displays it via the narrator (the say screen)
                    // in the dialogue box while the choices show. Only items WITH a block are choices.
                    if (!(t.Length > 2 && t[2] != null)) { captionId = p.Intern(capTxt == null ? "" : capTxt); continue; }
                    int cap = p.Intern(capTxt == null ? "" : capTxt);
                    string condTxt = AstNode.AsText(t[1]);
                    // -1 => always available. An uncompilable condition also maps to -1 (shown),
                    // preserving the old "all choices visible" behaviour; recorded in the report.
                    int cond = -1;
                    if (condTxt != null && condTxt != "True")
                    {
                        cond = p.CompileExpr(condTxt);
                        if (cond < 0) p.Unsupported.Add("menu-condition: " + condTxt);
                    }
                    List<AstNode> block = t.Length > 2 ? AstNode.ToNodes(t[2]) : new List<AstNode>();
                    choices.Add(new MenuChoice(cap, cond, block));
                }

            p.Emit(new Instr(IrOp.MenuStart, choices.Count, captionId));   // B = caption string id (-1 = none)
            List<Instr> choiceInstrs = new List<Instr>();
            foreach (MenuChoice c in choices) choiceInstrs.Add(p.Emit(new Instr(IrOp.Choice, c.Cap, c.Cond)));
            p.Emit(new Instr(IrOp.MenuEnd));

            List<Instr> endJumps = new List<Instr>();
            for (int idx = 0; idx < choices.Count; idx++)
            {
                choiceInstrs[idx].C = p.Here;                       // target = start of this choice's block
                foreach (AstNode cn in choices[idx].Block) Lower(cn, p);
                endJumps.Add(p.Emit(new Instr(IrOp.Jump)));
            }
            int end = p.Here;
            foreach (Instr j in endJumps) j.A = end;
        }

        private static void LowerIf(AstNode n, IrProgram p)
        {
            IList entries = n.Raw("entries") as IList;
            List<Instr> endJumps = new List<Instr>();
            if (entries != null)
                foreach (object e in entries)
                {
                    object[] t = e as object[];
                    if (t == null || t.Length < 2) continue;
                    string cond = AstNode.AsText(t[0]);
                    List<AstNode> block = AstNode.ToNodes(t[1]);

                    // Compile the condition to an expr program. If it can't be compiled
                    // (unsupported Python), fall through with no skip -- i.e. assume true and
                    // take this block, matching the old pre-evaluator behaviour. Recorded so
                    // the convert report flags it.
                    Instr skip = null;
                    if (cond != null && cond != "True")
                    {
                        int condExpr = p.CompileExpr(cond);
                        if (condExpr >= 0) skip = p.Emit(new Instr(IrOp.IfFalseGoto, condExpr));
                        else p.Unsupported.Add("if-condition: " + cond);
                    }

                    foreach (AstNode c in block) Lower(c, p);
                    endJumps.Add(p.Emit(new Instr(IrOp.Jump)));

                    if (skip != null) skip.C = p.Here;
                }
            int end = p.Here;
            foreach (Instr j in endJumps) j.A = end;
        }

        private static void LowerWhile(AstNode n, IrProgram p)
        {
            string cond = n.Text("condition");
            int condExpr = (cond == null || cond == "True") ? -1 : p.CompileExpr(cond);

            // `while True:` (condExpr -1 with cond True) loops with no guard. A condition we
            // can't compile would otherwise loop forever (the player can't test it), so skip
            // the loop body entirely (treat as false) rather than hang.
            if (cond != null && cond != "True" && condExpr < 0)
            {
                p.Unsupported.Add("while-condition: " + cond);
                Instr over = new Instr(IrOp.Jump);
                int patch = p.Here;
                p.Emit(over);
                foreach (AstNode c in n.Block("block")) Lower(c, p);
                over.A = p.Here;
                return;
            }

            int top = p.Here;
            Instr skip = (condExpr >= 0) ? p.Emit(new Instr(IrOp.IfFalseGoto, condExpr)) : null;
            foreach (AstNode c in n.Block("block")) Lower(c, p);
            Instr back = new Instr(IrOp.Jump); back.A = top; p.Emit(back);
            if (skip != null) skip.C = p.Here;
        }

        private static string CodeOf(AstNode n)
        {
            AstNode code = n.NodeAt("code");
            if (code != null) return code.PyCodeSource();
            return AstNode.AsText(n.Raw("code"));
        }

        // Matches a single simple assignment statement: a (dotted) name, an assignment operator
        // (=, +=, -=, *=, /=, %=; the (?!=) keeps `==` from matching), then a value expression.
        private static readonly Regex AssignStmt =
            new Regex(@"^\s*([A-Za-z_][A-Za-z0-9_.]*)\s*(\+=|-=|\*=|/=|%=|=(?!=))\s*(.+?)\s*$");

        // Lowers a few engine Python calls that drive control flow / menus into real ops the VM runs
        // (the PyExec copy stays a no-op, kept for Character() resolution):
        //   result = renpy.imagemap(ground, hover, [hotspots]) -> ImageMap op (interactive menu)
        //   renpy.jump_out_of_context("X") / renpy.jump("X")    -> Jump X
        //   renpy.quit()                                        -> End
        private static readonly Regex ImageMapAssign = new Regex(@"(\w+)\s*=\s*renpy\.imagemap\(", RegexOptions.Singleline);
        private static readonly Regex JumpCall = new Regex(@"renpy\.jump(?:_out_of_context)?\(\s*[uU]?[""']([^""']+)[""']\s*\)");
        private static readonly Regex QuitCall = new Regex(@"renpy\.quit\(\s*\)");

        private static void EmitNativeCalls(string src, IrProgram p)
        {
            if (string.IsNullOrEmpty(src)) return;
            Match im = ImageMapAssign.Match(src);
            if (im.Success)
            {
                int id = p.CompileImageMap(src);
                if (id >= 0) p.Emit(new Instr(IrOp.ImageMap, p.Intern(im.Groups[1].Value), id));
            }
            // Themed layout.imagemap_<screen>(...) calls define whole menu screens; collect them all
            // (looked up by Kind at runtime) rather than emitting a per-call op.
            p.CollectThemedImageMaps(src);
            foreach (Match jm in JumpCall.Matches(src))
            {
                Instr ins = new Instr(IrOp.Jump);
                ins.Sym = jm.Groups[1].Value;
                p.Emit(ins);
            }
            if (QuitCall.IsMatch(src)) p.Emit(new Instr(IrOp.End));

            // config.overlay_functions.append(NAME) / .remove(NAME) -> activate/deactivate a HUD overlay.
            foreach (Match am in OverlayAppend.Matches(src)) p.Emit(new Instr(IrOp.OverlayShow, p.Intern(am.Groups[1].Value)));
            foreach (Match rm in OverlayRemove.Matches(src)) p.Emit(new Instr(IrOp.OverlayHide, p.Intern(rm.Groups[1].Value)));

            // Capture any overlay FUNCTION definitions in this python block (def NAME(): ui.* ...).
            p.RegisterOverlays(src);
        }

        private static readonly Regex OverlayAppend = new Regex(@"config\.overlay_functions\.append\(\s*([A-Za-z_]\w*)\s*\)");
        private static readonly Regex OverlayRemove = new Regex(@"config\.overlay_functions\.remove\(\s*([A-Za-z_]\w*)\s*\)");

        // Lowers the simple `name = expr` statements inside a python block into Assign ops.
        // Statements that aren't plain assignments, or whose RHS won't compile, are skipped
        // (recorded as a fidelity NOTE -- the VM just won't change that variable; the PyExec
        // op still carries the source for Character() resolution). Only control-flow
        // conditions count as Unsupported, since those change which statements run.
        private static void EmitAssignments(string src, IrProgram p)
        {
            if (string.IsNullOrEmpty(src)) return;
            // Python blocks: one statement per line; also split simple `a=1; b=2` on ';'.
            string[] lines = src.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
            int defIndent = -1;   // when >=0 we're inside a def/class body (indent > defIndent) -> skip it:
                                  // those assignments are FUNCTION/METHOD bodies, not init code, so they
                                  // must not run at startup (e.g. toggle_auto's `afm_enable = not afm_enable`).
            foreach (string rawLine in lines)
            {
                string line = rawLine;
                int hash = IndexOfComment(line);
                if (hash >= 0) line = line.Substring(0, hash);
                if (line.Trim().Length == 0) continue;   // blank/comment-only: leave def-skip state as is
                int ind = 0; while (ind < line.Length && (line[ind] == ' ' || line[ind] == '\t')) ind++;
                if (defIndent >= 0)
                {
                    if (ind > defIndent) continue;   // still inside the def/class body -> skip
                    defIndent = -1;                  // dedented back out -> resume init assignments
                }
                string head = line.Substring(ind);
                if (head.StartsWith("def ") || head.StartsWith("class ")) { defIndent = ind; continue; }
                foreach (string stmt in line.Split(';'))
                {
                    Match m = AssignStmt.Match(stmt);
                    if (!m.Success) continue;
                    string var = m.Groups[1].Value;
                    string opTok = m.Groups[2].Value;
                    string rhs = m.Groups[3].Value;

                    int valExpr = p.CompileExpr(rhs);
                    if (valExpr < 0) { p.Notes.Add("assign-rhs not executable: " + stmt.Trim()); continue; }

                    int kind;
                    switch (opTok)
                    {
                        case "=": kind = 0; break;
                        case "+=": kind = 1; break;
                        case "-=": kind = 2; break;
                        case "*=": kind = 3; break;
                        case "/=": kind = 4; break;
                        case "%=": kind = 5; break;
                        default: continue;
                    }
                    p.Emit(new Instr(IrOp.Assign, p.Intern(var), kind, valExpr));
                }
            }
        }

        // Index of a `#` comment start that's outside any quotes, or -1.
        private static int IndexOfComment(string s)
        {
            char q = '\0';
            for (int i = 0; i < s.Length; i++)
            {
                char c = s[i];
                if (q != '\0') { if (c == q && (i == 0 || s[i - 1] != '\\')) q = '\0'; }
                else if (c == '\'' || c == '"') q = c;
                else if (c == '#') return i;
            }
            return -1;
        }

        // Map character variables (Say.who like "t") to their display name. Defined via
        // `define t = Character("Travis", ...)` (Default op) or `t = Character("Travis", ...)`
        // in an init python block (PyExec op). Character(None)/Character() => no name (narration).
        // Captures the variable and the Character() first argument (up to the first comma/paren).
        private static readonly Regex CharAssign =
            new Regex("([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*Character\\(\\s*([^,)]*)");
        private static readonly Regex CharFirstArg =
            new Regex("Character\\(\\s*([^,)]*)");
        private static readonly Regex QuotedStr =
            new Regex("[\"']([^\"']*)[\"']");

        // Resolves a Character() first argument to a display name. Returns false if it can't be
        // resolved statically (a variable/expression); name="" means narrator (no name shown).
        private static bool CharNameOf(string arg1, out string name)
        {
            name = "";
            arg1 = arg1.Trim();
            if (arg1.StartsWith("None")) return true;           // narrator / nameless
            Match q = QuotedStr.Match(arg1);                    // 'Name' / "Name" / _("Name")
            if (q.Success) { name = q.Groups[1].Value; return true; }
            return false;
        }

        private static readonly Regex CharVarDef = new Regex("([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*Character\\(");

        // True if a Character() definition string requests NVL mode (kind=nvl).
        private static bool IsNvlDef(string s) { return s != null && s.Replace(" ", "").IndexOf("kind=nvl", System.StringComparison.Ordinal) >= 0; }

        // Per-Character dialogue styling we can bake into the Say text: what_color (the `what` text
        // colour) and what_prefix (a speaker label baked in, e.g. for Character(None) MMO-chat lines).
        private static readonly Regex WhatColorRe  = new Regex("what_color\\s*=\\s*[\"']([^\"']+)[\"']");
        private static readonly Regex WhatPrefixRe = new Regex("what_prefix\\s*=\\s*[\"']([^\"']*)[\"']");

        // The full balanced Character(...) call text within `s` starting at the '(' index.
        private static string BalancedCall(string s, int openIdx)
        {
            int depth = 0;
            for (int k = openIdx; k < s.Length; k++)
            {
                if (s[k] == '(') depth++;
                else if (s[k] == ')') { depth--; if (depth == 0) return s.Substring(openIdx, k - openIdx + 1); }
            }
            return s.Substring(openIdx);
        }

        private static void ResolveCharacters(IrProgram p)
        {
            Dictionary<string, string> map = new Dictionary<string, string>(System.StringComparer.Ordinal);
            HashSet<string> nvlVars = new HashSet<string>(System.StringComparer.Ordinal);
            Dictionary<string, string> whatColor = new Dictionary<string, string>(System.StringComparer.Ordinal);
            Dictionary<string, string> whatPrefix = new Dictionary<string, string>(System.StringComparer.Ordinal);
            HashSet<string> ingameVars = new HashSet<string>(System.StringComparer.Ordinal);   // Character(window_background=..) chat boxes

            // Character definitions live in both the game flow AND the init prologue (top-level
            // `init python:` / `define`). Since the __init__ change those init defs are buffered into
            // p.InitCode and aren't merged into p.Code until after this pass runs -- so scan BOTH lists,
            // or every define is invisible and nothing (names, colours, nvl, in-game) resolves.
            List<Instr> defs = new List<Instr>();
            defs.AddRange(p.Code);
            defs.AddRange(p.InitCode);
            foreach (Instr ins in defs)
            {
                if (ins.Op == IrOp.Default)
                {
                    string val = p.Str(ins.B);
                    if (val.IndexOf("Character(", System.StringComparison.Ordinal) >= 0)
                    {
                        Match m = CharFirstArg.Match(val);
                        string nm;
                        if (m.Success && CharNameOf(m.Groups[1].Value, out nm)) map[p.Str(ins.A)] = nm;
                        if (IsNvlDef(val)) nvlVars.Add(p.Str(ins.A));
                        ScanCharStyle(p.Str(ins.A), val, whatColor, whatPrefix, ingameVars);
                    }
                }
                else if (ins.Op == IrOp.PyExec)
                {
                    string src = p.Str(ins.A);
                    foreach (Match m in CharAssign.Matches(src))
                    {
                        string nm;
                        if (CharNameOf(m.Groups[2].Value, out nm)) map[m.Groups[1].Value] = nm;
                        int open = src.IndexOf('(', m.Index);   // the full call -> what_color / what_prefix
                        if (open >= 0) ScanCharStyle(m.Groups[1].Value, BalancedCall(src, open), whatColor, whatPrefix, ingameVars);
                    }
                    // NVL: capture `var = Character(... kind=nvl ...)` per line.
                    foreach (string line in src.Split('\n'))
                    {
                        if (line.IndexOf("Character(", System.StringComparison.Ordinal) < 0 || !IsNvlDef(line)) continue;
                        Match v = CharVarDef.Match(line);
                        if (v.Success) nvlVars.Add(v.Groups[1].Value);
                    }
                }
            }
            if (map.Count == 0 && nvlVars.Count == 0 && whatColor.Count == 0 && whatPrefix.Count == 0 && ingameVars.Count == 0) return;

            foreach (Instr ins in p.Code)
            {
                if (ins.Op != IrOp.Say || ins.A < 0) continue;
                string who = p.Str(ins.A);
                if (nvlVars.Contains(who)) ins.C = 1;          // mark NVL line (player renders full-screen)
                else if (ingameVars.Contains(who)) ins.C = 2;  // mark in-game chat line (small fixed box + font)

                // Bake per-Character what_prefix + what_color into the displayed text (the renderer
                // handles {color=#..} tags). Done before who->name so the var is still known. For
                // Character(None) MMO-chat lines the prefix is the only speaker label (who stays -1).
                string pfx, clr;
                bool hasPfx = whatPrefix.TryGetValue(who, out pfx) && pfx.Length > 0;
                bool hasClr = whatColor.TryGetValue(who, out clr) && clr.Length > 0;
                if (hasPfx || hasClr)
                {
                    string txt = p.Str(ins.B);
                    if (hasPfx) txt = pfx + txt;
                    if (hasClr) txt = "{color=" + clr + "}" + txt + "{/color}";
                    ins.B = p.Intern(txt);
                }

                string name;
                if (map.TryGetValue(who, out name))
                    ins.A = (name.Length == 0) ? -1 : p.Intern(name);   // empty name => narration
            }
        }

        private static void ScanCharStyle(string var, string call, Dictionary<string, string> whatColor, Dictionary<string, string> whatPrefix, HashSet<string> ingameVars)
        {
            if (string.IsNullOrEmpty(var) || call == null) return;
            Match c = WhatColorRe.Match(call);
            if (c.Success) whatColor[var] = c.Groups[1].Value;
            Match pf = WhatPrefixRe.Match(call);
            if (pf.Success) whatPrefix[var] = pf.Groups[1].Value;
            // A window_background = a per-character box (the MMO chat speakers); render those lines in
            // the shared in-game textbox (Say kind=2) rather than the main ADV box.
            if (call.IndexOf("window_background", System.StringComparison.Ordinal) >= 0) ingameVars.Add(var);
        }

        private static void Resolve(IrProgram p)
        {
            foreach (Instr ins in p.Code)
            {
                if (ins.Sym == null) continue;
                // Keep the target NAME on Jumps (in B) so the player can route an unresolved jump to an
                // engine-generated screen label (e.g. load_screen / save_screen, created by Ren'Py's
                // layout.imagemap_load_save which we don't execute) to its own menu instead of going inert.
                if (ins.Op == IrOp.Jump) ins.B = p.Intern(ins.Sym);
                int addr;
                if (p.Labels.TryGetValue(ins.Sym, out addr)) ins.A = addr;
                else { ins.A = -1; p.Unresolved.Add(ins.Sym); }
            }
        }
    }
}
