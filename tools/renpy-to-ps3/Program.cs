using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Text;
using RenpyToPs3.RenPy;

namespace RenpyToPs3
{
   // usage: renpy-to-ps3 <command> [args...]
   //   list <rpa-file> | extract <rpa-file> <out> | info <game-dir>
   //   ast <rpyc> [label] | script <rpyc> | compile <rpyc|dir> [full|out.rbc]
   //   pack <game-dir> <out.rpk> [--max <px>] [--ascii-text] [--ffmpeg <path>] | rpk <file>
   internal static class Program
   {
      private static int Main(string[] args)
      {
         try
         {
            // Ren'Py scripts are UTF-8; emit UTF-8 regardless of console codepage.
            try { Console.OutputEncoding = Encoding.UTF8; } catch { }

            if (args.Length == 0) { PrintUsage(); return 0; }

            string command = args[0].ToLower();
            switch (command)
            {
               case "list":
                  if (args.Length < 2) { Console.Error.WriteLine("error: missing <rpa-file>"); PrintUsage(); return 1; }
                  return ListCommand(args[1]);

               case "extract":
                  if (args.Length < 3) { Console.Error.WriteLine("usage: renpy-to-ps3 extract <rpa-file> <output-dir>"); return 1; }
                  return ExtractCommand(args[1], args[2]);

               case "info":
                  if (args.Length < 2) { Console.Error.WriteLine("error: missing <game-dir>"); PrintUsage(); return 1; }
                  return InfoCommand(args[1]);

               case "ast":
                  if (args.Length < 2) { Console.Error.WriteLine("error: missing <rpyc-file>"); return 1; }
                  return AstCommand(args[1], args.Length > 2 ? args[2] : null);

               case "atldump":   // TEMP diagnostic: dump every renpy.atl.* node's fields
                  if (args.Length < 2) { Console.Error.WriteLine("error: missing <rpyc-file>"); return 1; }
                  return AtlDumpCommand(args[1]);

               case "script":
                  if (args.Length < 2) { Console.Error.WriteLine("error: missing <rpyc-file>"); return 1; }
                  return ScriptCommand(args[1]);

               case "compile":
                  if (args.Length < 2) { Console.Error.WriteLine("error: missing <rpyc-file-or-game-dir>"); return 1; }
                  return CompileCommand(args[1],
                      args.Length > 2 && args[2] == "full",
                      args.Length > 2 && args[2] != "full" ? args[2] : (args.Length > 3 ? args[3] : null));

               case "pack":
                  if (args.Length < 3) { Console.Error.WriteLine("usage: renpy-to-ps3 pack <game-dir> <out.rpk> [--max <px>] [--ascii-text] [--ffmpeg <path>] [--no-cache] [--clear-cache]"); return 1; }
                  return PackCommand(args);

               case "rpk":
                  if (args.Length < 2) { Console.Error.WriteLine("error: missing <rpk-file>"); return 1; }
                  return RpkCommand(args[1]);

               default:
                  Console.Error.WriteLine("error: unknown command '" + command + "'");
                  PrintUsage();
                  return 1;
            }
         }
         catch (Exception ex)
         {
            Console.Error.WriteLine("error: " + ex.Message);
            return 1;
         }
      }

      private static void PrintUsage()
      {
         Console.WriteLine("renpy-to-ps3 - Convert Ren'Py games to PS3 packages");
         Console.WriteLine();
         Console.WriteLine("Commands:");
         Console.WriteLine("  list <rpa-file>                         List archive contents");
         Console.WriteLine("  extract <rpa-file> <output>             Extract archive");
         Console.WriteLine("  info <game-dir>                         Show construct compatibility");
         Console.WriteLine("  compile <rpyc|game-dir> [out.rbc]       Compile to IR / bytecode");
         Console.WriteLine("  pack <game-dir> <out.rpk> [--max <px>]  Convert + compile + bundle");
         Console.WriteLine("       [--no-cache] [--clear-cache]       Asset cache: bypass / wipe-then-rebuild");
         Console.WriteLine("  rpk <file>                              Inspect an .rpk bundle");
         Console.WriteLine();
      }

      private static int ListCommand(string rpaPath)
      {
         if (!File.Exists(rpaPath)) { Console.Error.WriteLine("error: file not found: " + rpaPath); return 1; }

         RpaArchive rpa = new RpaArchive(rpaPath);
         Dictionary<string, List<RpaArchive.Segment>> index = rpa.ReadIndex();

         Console.WriteLine("version : " + rpa.Version);
         Console.WriteLine("index   : 0x" + rpa.IndexOffset.ToString("X"));
         Console.WriteLine("key     : 0x" + rpa.Key.ToString("X"));
         Console.WriteLine("files   : " + index.Count);
         Console.WriteLine();

         const int sample = 20;
         int shown = 0;
         foreach (KeyValuePair<string, List<RpaArchive.Segment>> kvp in index)
         {
            if (shown++ >= sample) break;
            Console.WriteLine("  " + kvp.Key + "  (" + rpa.FileSize(kvp.Value).ToString("N0") + " bytes)");
         }
         if (index.Count > sample) Console.WriteLine("  ... and " + (index.Count - sample) + " more");
         return 0;
      }

      private static int ExtractCommand(string rpaPath, string outputDir)
      {
         if (!File.Exists(rpaPath)) { Console.Error.WriteLine("error: file not found: " + rpaPath); return 1; }

         RpaArchive rpa = new RpaArchive(rpaPath);
         Dictionary<string, List<RpaArchive.Segment>> index = rpa.ReadIndex();

         Console.WriteLine("Extracting " + index.Count + " files from " + Path.GetFileName(rpaPath) + " ...");
         rpa.ExtractAll(outputDir, index, (count, total, name) =>
         {
             if (count % 50 == 0 || count == total) Console.WriteLine("  [" + count + "/" + total + "] " + name);
         });
         Console.WriteLine("Done. Extracted " + index.Count + " files to " + outputDir);
         return 0;
      }

      private static int InfoCommand(string gameDir)
      {
         if (!Directory.Exists(gameDir)) { Console.Error.WriteLine("error: directory not found: " + gameDir); return 1; }

         string[] files = Directory.GetFiles(gameDir, "*.rpyc", SearchOption.AllDirectories);
         if (files.Length == 0) { Console.Error.WriteLine("error: no .rpyc files found under " + gameDir); return 1; }

         HashSet<string> classes = new HashSet<string>(StringComparer.Ordinal);
         int scanned = 0, failed = 0;
         foreach (string f in files)
         {
            try { classes.UnionWith(PickleClassScanner.ScanClasses(RpycFile.LoadPickle(f))); scanned++; }
            catch (Exception ex) { failed++; Console.Error.WriteLine("  (skip) " + Path.GetFileName(f) + ": " + ex.Message); }
         }

         Dictionary<string, List<string>> buckets = new Dictionary<string, List<string>>();
         foreach (string c in classes) AddBucket(buckets, Classify(c), c);

         Console.WriteLine("game     : " + gameDir);
         Console.WriteLine("scanned  : " + scanned + " .rpyc file(s)" + (failed > 0 ? (", " + failed + " skipped") : ""));
         Console.WriteLine("constructs: " + classes.Count + " distinct classes referenced");
         Console.WriteLine();

         foreach (string cat in CategoryOrder)
         {
            List<string> set;
            if (!buckets.TryGetValue(cat, out set)) continue;
            set.Sort(StringComparer.Ordinal);
            Console.WriteLine(cat + ":");
            foreach (string c in set) Console.WriteLine("    " + c);
            Console.WriteLine();
         }

         bool screens = buckets.ContainsKey(CatScreen);
         bool atl = buckets.ContainsKey(CatAtl);
         bool other = buckets.ContainsKey(CatOther);
         Console.WriteLine("verdict:");
         if (!screens && !atl)
            Console.WriteLine("    Linear core only - strong early-conversion candidate.");
         else
         {
            Console.WriteLine("    Linear story is convertible, but full fidelity needs:");
            if (atl) Console.WriteLine("      - ATL / transforms (sprite animation)");
            if (screens) Console.WriteLine("      - screen language (menus / GUI - the 'looks identical' bar)");
         }
         if (other) Console.WriteLine("    NOTE: unrecognized classes present (see 'Other / review') - confirm before converting.");
         return 0;
      }

      private static int AstCommand(string rpycPath, string labelName)
      {
         if (!File.Exists(rpycPath)) { Console.Error.WriteLine("error: file not found: " + rpycPath); return 1; }

         object root = RpycFile.LoadAst(rpycPath);
         Console.WriteLine("top-level: " + Shape(root));

         IList stmts = RpycFile.FindStatementList(root);
         if (stmts == null) { Console.WriteLine("(could not locate a statement list)"); return 0; }
         Console.WriteLine("statements: " + stmts.Count);

         SortedDictionary<string, int> topHist = new SortedDictionary<string, int>(StringComparer.Ordinal);
         foreach (AstNode s in AstNode.ToNodes(stmts)) Inc(topHist, s.Short);
         Console.WriteLine("\n-- TOP-LEVEL class counts --");
         foreach (KeyValuePair<string, int> kv in topHist) Console.WriteLine(string.Format("  {0,5}  {1}", kv.Value, kv.Key));

         SortedDictionary<string, int> hist = new SortedDictionary<string, int>(StringComparer.Ordinal);
         HashSet<object> seen = new HashSet<object>(RefComparer.Instance);
         CountClasses(root, hist, seen, 0);
         Console.WriteLine("\n-- WHOLE-TREE class histogram --");
         foreach (KeyValuePair<string, int> kv in hist) Console.WriteLine(string.Format("  {0,5}  {1}", kv.Value, kv.Key));

         if (labelName != null)
         {
            Console.WriteLine("\n-- locating label '" + labelName + "' --");
            int topMatch = 0;
            foreach (AstNode s in AstNode.ToNodes(stmts))
               if (s.Short == "Label" && s.LabelName() == labelName)
               {
                  topMatch++;
                  List<AstNode> block = s.Block("block");
                  List<string> kinds = new List<string>();
                  for (int k = 0; k < block.Count && k < 8; k++) kinds.Add(block[k].Short);
                  Console.WriteLine("  found at TOP LEVEL; block has " + block.Count + " statements: " + string.Join(", ", kinds.ToArray()));
               }
            if (topMatch == 0) Console.WriteLine("  NOT found among top-level statements (it is nested somewhere).");
         }
         return 0;
      }

      // TEMP diagnostic: walk the whole AST and print each renpy.atl.* node's state dict (recursively),
      // so we can see the real field names + keyframe contents.
      private static int AtlDumpCommand(string rpycPath)
      {
         if (!File.Exists(rpycPath)) { Console.Error.WriteLine("error: file not found: " + rpycPath); return 1; }
         object root = RpycFile.LoadAst(rpycPath);
         AtlWalk(root, new HashSet<object>(RefComparer.Instance), 0, false);
         return 0;
      }

      private static void AtlWalk(object o, HashSet<object> seen, int depth, bool insideAtl)
      {
         if (o == null || depth > 40) return;
         PyObject p = o as PyObject;
         if (p != null)
         {
            if (!seen.Add(p)) return;
            bool isAtl = p.ClassName != null && p.ClassName.IndexOf(".atl.", StringComparison.Ordinal) >= 0;
            if (isAtl)
            {
               Console.WriteLine(new string(' ', depth * 2) + "<" + p.ClassName + ">");
               IDictionary st = (p.State as object[] != null && ((object[])p.State).Length >= 2) ? ((object[])p.State)[1] as IDictionary : (p.State as IDictionary);
               if (st == null && p.State is IDictionary) st = (IDictionary)p.State;
               if (st != null)
                  foreach (DictionaryEntry de in st)
                     Console.WriteLine(new string(' ', depth * 2 + 2) + de.Key + " = " + Render(de.Value, 3));
            }
            // recurse into state + args to find nested atl nodes
            AtlWalk(p.State, seen, depth + (isAtl ? 1 : 0), insideAtl || isAtl);
            if (p.Args != null) foreach (object a in p.Args) AtlWalk(a, seen, depth + (isAtl ? 1 : 0), insideAtl || isAtl);
            return;
         }
         object[] tup = o as object[];
         if (tup != null) { foreach (object it in tup) AtlWalk(it, seen, depth, insideAtl); return; }
         IDictionary d = o as IDictionary;
         if (d != null) { foreach (DictionaryEntry de in d) AtlWalk(de.Value, seen, depth, insideAtl); return; }
         IList l = o as IList;
         if (l != null) { foreach (object it in l) AtlWalk(it, seen, depth, insideAtl); return; }
      }

      // Compact recursive renderer for diagnostic values (a few levels deep).
      private static string Render(object o, int depth)
      {
         if (o == null) return "None";
         string s = o as string;
         if (s != null) return "\"" + s + "\"";
         PyObject p = o as PyObject;
         if (p != null)
         {
            string txt = AstNode.AsText(p);
            if (txt != null) return "expr(\"" + txt + "\")";
            return "<" + p.ClassName + ">";
         }
         if (depth <= 0) return Shape(o);
         object[] tup = o as object[];
         if (tup != null)
         {
            string[] parts = new string[tup.Length];
            for (int i = 0; i < tup.Length; i++) parts[i] = Render(tup[i], depth - 1);
            return "(" + string.Join(", ", parts) + ")";
         }
         IList l = o as IList;
         if (l != null)
         {
            System.Text.StringBuilder sb = new System.Text.StringBuilder("[");
            for (int i = 0; i < l.Count; i++) { if (i > 0) sb.Append(", "); sb.Append(Render(l[i], depth - 1)); }
            sb.Append("]");
            return sb.ToString();
         }
         return o.ToString();
      }

      private static string Shape(object o)
      {
         if (o == null) return "null";
         string s = o as string;
         if (s != null) return "\"" + (s.Length > 50 ? s.Substring(0, 50) + "..." : s) + "\"";
         PyObject p = o as PyObject;
         if (p != null) return "<" + p.ClassName + " state=" + ShapeState(p.State) + ">";
         GlobalRef g = o as GlobalRef;
         if (g != null) return "global:" + g.Full;
         object[] a = o as object[];
         if (a != null) return "tuple[" + a.Length + "]";
         IDictionary d = o as IDictionary;
         if (d != null) return "dict{" + d.Count + "}";
         IList l = o as IList;
         if (l != null) return "list[" + l.Count + "]";
         return o.GetType().Name + "=" + o;
      }

      private static string ShapeState(object state)
      {
         if (state == null) return "null";
         object[] a = state as object[];
         if (a != null)
         {
            string extra = "";
            if (a.Length > 1) { IDictionary d2 = a[1] as IDictionary; if (d2 != null) extra = " attrs{" + d2.Count + "}"; }
            return "tuple[" + a.Length + "]" + extra;
         }
         IDictionary d = state as IDictionary;
         if (d != null) return "dict{" + d.Count + "}";
         return state.GetType().Name;
      }

      private static int PackCommand(string[] args)
      {
         string gameDir = args[1], outRpk = args[2];
         int maxDim = 1920;            // single max-edge knob so the tool can target PS3/Vita/PSP
         string ffmpegPath = null;
         bool asciiText = false;       // normalize curly quotes/ellipsis to ASCII (system-font fallback)
         bool useCache = true;         // reuse encoded assets across packs (keyed by content + params)
         bool clearCache = false;      // wipe the cache before this pack (force a clean re-encode)
         for (int i = 3; i < args.Length; i++)
         {
            if (args[i] == "--max" && i + 1 < args.Length) { int.TryParse(args[++i], out maxDim); }
            else if (args[i] == "--ffmpeg" && i + 1 < args.Length) ffmpegPath = args[++i];
            else if (args[i] == "--ascii-text") asciiText = true;
            else if (args[i] == "--no-cache") useCache = false;        // re-encode everything (no cache, no passthrough)
            else if (args[i] == "--clear-cache") clearCache = true;    // drop the cache first, then pack + repopulate
         }
         if (maxDim < 16) { Console.Error.WriteLine("error: --max too small"); return 1; }
         if (!Directory.Exists(gameDir)) { Console.Error.WriteLine("error: game dir not found: " + gameDir); return 1; }

         Ffmpeg ff;
         try { ff = new Ffmpeg(ffmpegPath); }
         catch (Exception ex) { Console.Error.WriteLine("error: " + ex.Message); return 1; }

         TextWriter original = Console.Out;
         using (StreamWriter logw = new StreamWriter(outRpk + ".log"))
         {
            logw.AutoFlush = true;
            Console.SetOut(new TeeWriter(original, logw));
            try
            {
               Console.WriteLine("packing " + gameDir + " -> " + outRpk + "  (max edge " + maxDim + "px, ascii-text=" + asciiText + ", ffmpeg: " + ff.Path + ")");
               int rc = Packer.Pack(gameDir, outRpk, ff, maxDim, asciiText, useCache, clearCache);
               Console.WriteLine("PACK_DONE rc=" + rc);
               return rc;
            }
            finally { Console.SetOut(original); }
         }
      }

      private sealed class TeeWriter : TextWriter
      {
         private readonly TextWriter a, b;
         public TeeWriter(TextWriter a, TextWriter b) { this.a = a; this.b = b; }
         public override Encoding Encoding { get { return a.Encoding; } }
         public override void Write(char c) { a.Write(c); b.Write(c); }
         public override void Write(string s) { a.Write(s); b.Write(s); }
         public override void WriteLine(string s) { a.WriteLine(s); b.WriteLine(s); }
      }

      private static int RpkCommand(string path)
      {
         if (!File.Exists(path)) { Console.Error.WriteLine("error: file not found: " + path); return 1; }
         List<RpkTocEntry> toc = Rpk.ReadToc(path);
         long total = 0;
         foreach (RpkTocEntry e in toc) total += e.Length;
         toc.Sort((x, y) => y.Length.CompareTo(x.Length));

         Console.WriteLine(path + ": " + toc.Count + " entries");
         for (int i = 0; i < toc.Count && i < 25; i++)
            Console.WriteLine(string.Format("  {0,12:N0}  {1}", toc[i].Length, toc[i].Name));
         if (toc.Count > 25) Console.WriteLine("  ... and " + (toc.Count - 25) + " more");
         Console.WriteLine("total payload: " + total.ToString("N0") + " bytes; file: " + new FileInfo(path).Length.ToString("N0") + " bytes");
         return 0;
      }

      private static void CountClasses(object o, SortedDictionary<string, int> hist, HashSet<object> seen, int depth)
      {
         if (o == null || depth > 200) return;
         PyObject p = o as PyObject;
         if (p != null)
         {
            if (!seen.Add(p)) return;
            Inc(hist, p.ClassName);
            CountClasses(p.State, hist, seen, depth + 1);
            if (p.ListItems != null) foreach (object it in p.ListItems) CountClasses(it, hist, seen, depth + 1);
            if (p.DictItems != null) foreach (object v in p.DictItems.Values) CountClasses(v, hist, seen, depth + 1);
            if (p.Args != null) foreach (object a in p.Args) CountClasses(a, hist, seen, depth + 1);
            return;
         }
         object[] tup = o as object[];
         if (tup != null) { foreach (object it in tup) CountClasses(it, hist, seen, depth + 1); return; }
         IDictionary d = o as IDictionary;
         if (d != null)
         {
            if (!seen.Add(d)) return;
            foreach (DictionaryEntry e in d) CountClasses(e.Value, hist, seen, depth + 1);
            return;
         }
         IList l = o as IList;
         if (l != null)
         {
            if (!seen.Add(l)) return;
            foreach (object it in l) CountClasses(it, hist, seen, depth + 1);
         }
      }

      private static int ScriptCommand(string rpycPath)
      {
         if (!File.Exists(rpycPath)) { Console.Error.WriteLine("error: file not found: " + rpycPath); return 1; }
         IList stmts = RpycFile.LoadStatements(rpycPath);
         if (stmts == null) { Console.WriteLine("(no statement list)"); return 1; }
         StringBuilder sb = new StringBuilder();
         foreach (AstNode node in AstNode.ToNodes(stmts)) Render(node, 0, sb);
         Console.Write(sb.ToString());
         return 0;
      }

      private static void RenderBlock(List<AstNode> block, int indent, StringBuilder sb)
      {
         foreach (AstNode n in block) Render(n, indent, sb);
      }

      private static void Render(AstNode n, int indent, StringBuilder sb)
      {
         string pad = new string(' ', indent * 4);
         switch (n.Short)
         {
            case "Label":
               sb.Append(pad).Append("label ").Append(n.LabelName()).Append(":\n");
               RenderBlock(n.Block("block"), indent + 1, sb);
               break;
            case "Say":
               {
                  string who = n.Text("who");
                  string what = n.Text("what"); if (what == null) what = "";
                  sb.Append(pad).Append(who != null ? who + " " : "").Append('"').Append(what).Append("\"\n");
                  break;
               }
            case "Jump": sb.Append(pad).Append("jump ").Append(n.Text("target")).Append('\n'); break;
            case "Call": sb.Append(pad).Append("call ").Append(n.Text("label")).Append('\n'); break;
            case "Return": sb.Append(pad).Append("return\n"); break;
            case "Pass": sb.Append(pad).Append("pass\n"); break;
            case "Scene": sb.Append(pad).Append("scene ").Append(n.ImspecName()).Append('\n'); break;
            case "Show": sb.Append(pad).Append("show ").Append(n.ImspecName()).Append('\n'); break;
            case "Hide": sb.Append(pad).Append("hide ").Append(n.ImspecName()).Append('\n'); break;
            case "With": sb.Append(pad).Append("with ").Append(n.Text("expr")).Append('\n'); break;
            case "Menu":
               {
                  sb.Append(pad).Append("menu:\n");
                  IList items = n.Raw("items") as IList;
                  if (items != null)
                     foreach (object item in items)
                     {
                        object[] t = item as object[];
                        if (t == null || t.Length < 2) continue;
                        string caption = AstNode.AsText(t[0]);
                        string cond = AstNode.AsText(t[1]);
                        sb.Append(pad).Append("    \"").Append(caption).Append('"');
                        if (cond != null && cond != "True") sb.Append(" if ").Append(cond);
                        sb.Append(":\n");
                        if (t.Length > 2) RenderBlock(AstNode.ToNodes(t[2]), indent + 2, sb);
                     }
                  break;
               }
            case "If":
               {
                  bool first = true;
                  IList entries = n.Raw("entries") as IList;
                  if (entries != null)
                     foreach (object entry in entries)
                     {
                        object[] t = entry as object[];
                        if (t == null || t.Length < 2) continue;
                        string cond = AstNode.AsText(t[0]);
                        sb.Append(pad).Append(first ? "if " : "elif ").Append(cond).Append(":\n");
                        RenderBlock(AstNode.ToNodes(t[1]), indent + 1, sb);
                        first = false;
                     }
                  break;
               }
            case "Python":
            case "Init":
               {
                  string src = n.PyCodeSource();
                  if (src != null)
                  {
                     bool oneLine = src.IndexOf('\n') < 0;
                     sb.Append(pad).Append(oneLine ? "$ " + src.Trim() : "python: ...").Append('\n');
                  }
                  else { sb.Append(pad).Append("# <").Append(n.Short).Append(">\n"); RenderBlock(n.Block("block"), indent + 1, sb); }
                  break;
               }
            case "UserStatement": sb.Append(pad).Append(n.Text("line")).Append('\n'); break;
            default: sb.Append(pad).Append("# <").Append(n.Short).Append(">\n"); break;
         }
      }

      private static int CompileCommand(string path, bool full, string outRbc)
      {
         List<IList> units = new List<IList>();
         if (Directory.Exists(path))
         {
            foreach (string f in Directory.GetFiles(path, "*.rpyc", SearchOption.TopDirectoryOnly))
            {
               try { IList list = RpycFile.LoadStatements(f); if (list != null) units.Add(list); }
               catch (Exception ex) { Console.Error.WriteLine("  (skip) " + Path.GetFileName(f) + ": " + ex.Message); }
            }
            Console.WriteLine("compiling " + units.Count + " .rpyc file(s) from " + path);
         }
         else if (File.Exists(path))
         {
            IList list = RpycFile.LoadStatements(path);
            if (list == null) { Console.Error.WriteLine("error: no statement list"); return 1; }
            units.Add(list);
         }
         else { Console.Error.WriteLine("error: not found: " + path); return 1; }

         IrProgram prog = Compiler.CompileUnits(units);

         if (full)
            for (int a = 0; a < prog.Code.Count; a++)
               Console.WriteLine(string.Format("{0,5}: {1}", a, FmtInstr(prog, prog.Code[a])));

         SortedDictionary<string, int> ops = new SortedDictionary<string, int>(StringComparer.Ordinal);
         foreach (Instr ins in prog.Code) Inc(ops, ins.Op.ToString());

         Console.WriteLine("\n== IR summary ==");
         Console.WriteLine("instructions : " + prog.Code.Count);
         Console.WriteLine("strings      : " + prog.Strings.Count);
         Console.WriteLine("labels       : " + prog.Labels.Count);
         Console.WriteLine("atl programs : " + prog.Atls.Count);
         Console.WriteLine("imagemaps    : " + prog.ImageMaps.Count);
         foreach (RenpyToPs3.RenPy.ImageMapDef imd in prog.ImageMaps)
         {
            string kind = string.IsNullOrEmpty(imd.Kind) ? "(simple)" : imd.Kind;
            Console.WriteLine("    [" + kind + "] ground=" + imd.Ground
                + " idle=" + imd.Idle + " hover=" + imd.Hover
                + " selIdle=" + imd.SelectedIdle + " selHover=" + imd.SelectedHover
                + "  hotspots=" + imd.Hotspots.Count);
            System.Text.StringBuilder names = new System.Text.StringBuilder();
            foreach (RenpyToPs3.RenPy.ImageMapHotspot hh in imd.Hotspots)
               names.Append(hh.Name).Append(' ');
            Console.WriteLine("        names: " + names.ToString().Trim());
         }
         Console.WriteLine("overlays     : " + prog.Overlays.Count);
         Console.WriteLine("op counts    :");
         foreach (KeyValuePair<string, int> kv in ops) Console.WriteLine(string.Format("    {0,5}  {1}", kv.Value, kv.Key));

         // unsupported by frequency (desc)
         SortedDictionary<string, int> unsupCount = new SortedDictionary<string, int>(StringComparer.Ordinal);
         foreach (string u in prog.Unsupported) Inc(unsupCount, u);
         List<KeyValuePair<string, int>> unsupList = new List<KeyValuePair<string, int>>(unsupCount);
         unsupList.Sort((x, y) => y.Value.CompareTo(x.Value));
         Console.WriteLine("\nunsupported nodes : " + (prog.Unsupported.Count == 0 ? "none" : prog.Unsupported.Count.ToString()));
         foreach (KeyValuePair<string, int> g in unsupList) Console.WriteLine(string.Format("    {0,5}  {1}", g.Value, g.Key));

         // non-fatal fidelity notes (don't affect the verdict)
         SortedDictionary<string, int> noteCount = new SortedDictionary<string, int>(StringComparer.Ordinal);
         foreach (string nt in prog.Notes) Inc(noteCount, nt);
         Console.WriteLine("fidelity notes    : " + (prog.Notes.Count == 0 ? "none" : prog.Notes.Count.ToString()));
         foreach (KeyValuePair<string, int> g in noteCount) Console.WriteLine(string.Format("    {0,5}  {1}", g.Value, g.Key));

         Console.WriteLine("\n== GUI manifest (game.gui) ==");
         string guiTxt = GuiManifest.Build(prog);
         Console.Write(guiTxt.Length == 0 ? "    (no GUI settings found)\n" : guiTxt);

         List<string> unres = new List<string>(new HashSet<string>(prog.Unresolved));
         unres.Sort(StringComparer.Ordinal);
         Console.WriteLine("unresolved jump/call targets : " + (unres.Count == 0 ? "none" : unres.Count.ToString()));
         for (int k = 0; k < unres.Count && k < 20; k++) Console.WriteLine("    " + unres[k]);

         bool ok = prog.Unsupported.Count == 0 && unres.Count == 0;
         Console.WriteLine("\nverdict: " + (ok
             ? "fully lowered to linear-core IR."
             : "NOT fully convertible yet (see unsupported / unresolved above)."));

         if (outRbc != null)
         {
            byte[] bytes = Bytecode.Write(prog);
            File.WriteAllBytes(outRbc, bytes);
            Bytecode.Decoded dec = Bytecode.Read(bytes);
            int st;
            uint expectEntry = (uint)(prog.Labels.TryGetValue("start", out st) ? st : 0);
            bool rt = dec.Code.Count == prog.Code.Count
                    && dec.Strings.Count == prog.Strings.Count
                    && dec.Labels.Count == prog.Labels.Count
                    && dec.EntryAddr == expectEntry;
            for (int k = 0; rt && k < prog.Code.Count; k++)
            {
               Instr x = prog.Code[k]; Instr y = dec.Code[k];
               if (x.Op != y.Op || x.A != y.A || x.B != y.B || x.C != y.C) rt = false;
            }
            for (int k = 0; rt && k < prog.Strings.Count; k++)
               if (prog.Strings[k] != dec.Strings[k]) rt = false;

            Console.WriteLine("\nwrote " + outRbc + " (" + bytes.Length.ToString("N0") + " bytes)");
            Console.WriteLine("round-trip verify: " + (rt ? "PASS (decoded == compiled)" : "FAIL"));
            Console.WriteLine("entry 'start' @ " + dec.EntryAddr);
         }
         return 0;
      }

      private static string Prev(string s)
      {
         s = s.Replace("\n", " ").Replace("\r", "");
         return s.Length > 50 ? s.Substring(0, 50) + "..." : s;
      }

      private static string FmtInstr(IrProgram p, Instr i)
      {
         switch (i.Op)
         {
            case IrOp.Label: return "label " + p.Str(i.A) + ":";
            case IrOp.Say: return "SAY " + (i.A >= 0 ? p.Str(i.A) + " " : "") + "\"" + Prev(p.Str(i.B)) + "\"";
            case IrOp.Scene: return "SCENE " + p.Str(i.A);
            case IrOp.Show: return "SHOW " + p.Str(i.A);
            case IrOp.Hide: return "HIDE " + p.Str(i.A);
            case IrOp.With: return "WITH " + p.Str(i.A);
            case IrOp.Jump: return "JUMP -> " + i.A;
            case IrOp.Call: return "CALL -> " + i.A;
            case IrOp.Return: return "RETURN";
            case IrOp.MenuStart: return "MENU (" + i.A + " choices)";
            case IrOp.Choice: return "  CHOICE \"" + Prev(p.Str(i.A)) + "\"" + (i.B >= 0 ? " if " + FmtExpr(p, i.B) : "") + " -> " + i.C;
            case IrOp.MenuEnd: return "MENUEND";
            case IrOp.IfFalseGoto: return "IF NOT (" + FmtExpr(p, i.A) + ") GOTO " + i.C;
            case IrOp.PyExec: return "PY " + Prev(p.Str(i.A));
            case IrOp.Default: return "DEFAULT " + p.Str(i.A) + " = " + (i.C >= 0 ? FmtExpr(p, i.C) : Prev(p.Str(i.B)));
            case IrOp.Assign: return "ASSIGN " + p.Str(i.A) + " " + AssignOpStr(i.B) + " " + FmtExpr(p, i.C);
            case IrOp.Image: return "IMAGE " + p.Str(i.A) + " = " + Prev(p.Str(i.B));
            case IrOp.User: return "USER " + Prev(p.Str(i.A));
            default: return i.Op.ToString();
         }
      }

      private static string AssignOpStr(int kind)
      {
         switch (kind) { case 1: return "+="; case 2: return "-="; case 3: return "*="; case 4: return "/="; case 5: return "%="; default: return "="; }
      }

      // Renders a compiled RPN expr program back into a readable postfix string for `script`.
      private static string FmtExpr(IrProgram p, int idx)
      {
         if (idx < 0 || idx >= p.Exprs.Count) return "?expr#" + idx;
         System.Text.StringBuilder sb = new System.Text.StringBuilder();
         foreach (ExprInstr e in p.Exprs[idx].Ops)
         {
            if (sb.Length > 0) sb.Append(' ');
            switch (e.Op)
            {
               case ExprOp.PushInt: sb.Append(e.Arg); break;
               case ExprOp.PushBool: sb.Append(e.Arg != 0 ? "True" : "False"); break;
               case ExprOp.PushNone: sb.Append("None"); break;
               case ExprOp.PushFloat: sb.Append(p.Str(e.Arg)); break;
               case ExprOp.PushStr: sb.Append('"').Append(p.Str(e.Arg)).Append('"'); break;
               case ExprOp.LoadVar: sb.Append(p.Str(e.Arg)); break;
               default: sb.Append(e.Op.ToString().ToLowerInvariant()); break;
            }
         }
         return sb.ToString();
      }

      private static void Inc(SortedDictionary<string, int> d, string key)
      {
         int v; d.TryGetValue(key, out v); d[key] = v + 1;
      }

      private static void AddBucket(Dictionary<string, List<string>> b, string cat, string cls)
      {
         List<string> l;
         if (!b.TryGetValue(cat, out l)) { l = new List<string>(); b[cat] = l; }
         l.Add(cls);
      }

      private const string CatCore = "Supported (linear core)";
      private const string CatPy = "Supported (python / data)";
      private const string CatUser = "Partial (user statements - depends on verb)";
      private const string CatAtl = "Deferred (ATL / transforms - animation)";
      private const string CatScreen = "Not yet supported (screen language / display)";
      private const string CatTl = "Deferred (translations)";
      private const string CatOther = "Other / review";

      private static readonly string[] CategoryOrder = { CatCore, CatPy, CatUser, CatAtl, CatScreen, CatTl, CatOther };

      private static readonly HashSet<string> CoreNodes = new HashSet<string>(StringComparer.Ordinal)
      {
         "renpy.ast.Say", "renpy.ast.Scene", "renpy.ast.Show", "renpy.ast.Hide", "renpy.ast.With",
         "renpy.ast.Jump", "renpy.ast.Call", "renpy.ast.Return", "renpy.ast.Label", "renpy.ast.Menu",
         "renpy.ast.Pass", "renpy.ast.Init", "renpy.ast.If", "renpy.ast.While",
         "renpy.ast.Default", "renpy.ast.Define", "renpy.ast.Image", "renpy.ast.EarlyPython",
         "renpy.ast.Python", "renpy.ast.PyCode", "renpy.ast.PyExpr"
      };

      private static string Classify(string cls)
      {
         if (CoreNodes.Contains(cls)) return CatCore;
         if (cls.EndsWith(".PyExpr", StringComparison.Ordinal) || cls.EndsWith(".PyCode", StringComparison.Ordinal)) return CatCore;
         if (cls == "renpy.ast.UserStatement") return CatUser;
         if (cls.StartsWith("renpy.atl.", StringComparison.Ordinal) || cls == "renpy.ast.Transform") return CatAtl;
         if (cls.IndexOf("Translate", StringComparison.Ordinal) >= 0) return CatTl;
         if (cls.StartsWith("renpy.sl2.", StringComparison.Ordinal) ||
             cls.StartsWith("renpy.display.", StringComparison.Ordinal) ||
             cls.StartsWith("renpy.ui.", StringComparison.Ordinal) ||
             cls.StartsWith("renpy.text.", StringComparison.Ordinal) ||
             cls == "renpy.ast.Screen" || cls == "renpy.ast.Style")
            return CatScreen;
         if (cls.StartsWith("renpy.python.", StringComparison.Ordinal) ||
             cls.StartsWith("renpy.object.", StringComparison.Ordinal) ||
             cls.StartsWith("renpy.revertable.", StringComparison.Ordinal) ||
             cls.StartsWith("renpy.parameter.", StringComparison.Ordinal) ||
             cls.StartsWith("builtins.", StringComparison.Ordinal) ||
             cls.StartsWith("collections.", StringComparison.Ordinal) ||
             cls == "renpy.ast.ArgumentInfo" || cls == "renpy.ast.ParameterInfo")
            return CatPy;
         return CatOther;
      }
   }
}
