using System.Collections.Generic;

namespace RenpyToPs3.RenPy
{
   // IR opcodes: a flat, explicit instruction set the PS3 runtime can interpret without
   // understanding Ren'Py's AST. Expression-bearing ops (PyExec, IfFalseGoto, Default...)
   // carry an interned source-string id for now; a later expression evaluator consumes it.
   public enum IrOp
   {
      Label,        // A=name str id
      Say,          // A=who str id (-1 none)   B=text str id
      Scene,        // A=image name str id
      Show,         // A=image name str id
      Hide,         // A=image name str id
      With,         // A=transition expr str id
      Jump,         // A=target addr (resolved)
      Call,         // A=target addr (resolved)
      Return,
      MenuStart,    // A=choice count
      Choice,       // A=caption str id  B=condition expr id (-1 none)  C=target addr
      MenuEnd,
      IfFalseGoto,  // A=condition expr id (-1 => always true)  C=target addr (jump if false)
      PyExec,       // A=python source str id (kept for Character() name resolution)
      Default,      // A=varname str id  B=value-expr str id  C=value expr id (-1 if none)
      Image,        // A=image name str id  B=definition str id
      User,         // A=user-statement line str id
      Pause,
      Assign,       // A=varname str id  B=op kind (0:= 1:+= 2:-= 3:*= 4:/= 5:%=)  C=value expr id
      Nop,
      End,
      ImageMap,     // A=result varname str id  B=imagemap table id (interactive menu)
      OverlayShow,  // A=overlay name str id (config.overlay_functions.append)
      OverlayHide   // A=overlay name str id (config.overlay_functions.remove)
   }

   public sealed class Instr
   {
      public IrOp Op;
      public int A, B, C;
      public string Sym;   // unresolved symbolic jump/call target (label name) until patched into A

      public Instr(IrOp op) { Op = op; A = -1; B = -1; C = -1; }
      public Instr(IrOp op, int a) { Op = op; A = a; B = -1; C = -1; }
      public Instr(IrOp op, int a, int b) { Op = op; A = a; B = b; C = -1; }
      public Instr(IrOp op, int a, int b, int c) { Op = op; A = a; B = b; C = c; }
   }

   // A compiled program: code, a deduplicated string table, label map, and reports.
   public sealed class IrProgram
   {
      public readonly List<Instr> Code = new List<Instr>();
      public readonly List<string> Strings = new List<string>();
      public readonly Dictionary<string, int> Labels = new Dictionary<string, int>(System.StringComparer.Ordinal);
      public readonly List<string> Unsupported = new List<string>();
      public readonly List<string> Unresolved = new List<string>();

      // Non-fatal fidelity notes: constructs we lower but render approximately (e.g. say
      // attributes). Printed in the convert report; they do not affect the verdict.
      public readonly List<string> Notes = new List<string>();

      // Compiled RPN expression programs, referenced by index from condition/assignment ops.
      public readonly List<ExprProgram> Exprs = new List<ExprProgram>();
      private readonly Dictionary<string, int> _exprIntern = new Dictionary<string, int>(System.StringComparer.Ordinal);

      // Compiled ATL keyframe programs, referenced by index (the C field) from Show/Scene/Image ops.
      public readonly List<AtlProgram> Atls = new List<AtlProgram>();
      private readonly Dictionary<string, int> _atlIntern = new Dictionary<string, int>(System.StringComparer.Ordinal);

      // Parsed imagemap menus, referenced by index (the B field) from ImageMap ops.
      public readonly List<ImageMapDef> ImageMaps = new List<ImageMapDef>();
      private readonly Dictionary<string, int> _imIntern = new Dictionary<string, int>(System.StringComparer.Ordinal);

      // Parsed overlay (HUD) functions, keyed by name; OverlayShow/Hide ops reference them by name.
      public readonly Dictionary<string, OverlayDef> Overlays = new Dictionary<string, OverlayDef>(System.StringComparer.Ordinal);
      public void RegisterOverlays(string pySrc) { OverlayCompiler.ParseAll(pySrc, this, Overlays); }

      private readonly Dictionary<string, int> _intern = new Dictionary<string, int>(System.StringComparer.Ordinal);

      public int Intern(string s)
      {
         if (s == null) s = "";
         int i;
         if (_intern.TryGetValue(s, out i)) return i;
         i = Strings.Count;
         Strings.Add(s);
         _intern[s] = i;
         return i;
      }

      public string Str(int id)
      {
         return (id >= 0 && id < Strings.Count) ? Strings[id] : "";
      }

      // Init prologue: top-level / init-block ops are buffered here and appended under an `__init__`
      // label the player runs once at startup (Ren'Py's init phase, which our VM otherwise skips by
      // jumping straight to start/main_menu). These ops are flat (assigns/overlay/pyexec/default; no
      // jumps), so they carry no internal addresses.
      public readonly List<Instr> InitCode = new List<Instr>();
      private bool _toInit;
      public void BeginInit() { _toInit = true; }
      public void EndInit() { _toInit = false; }

      public Instr Emit(Instr instr) { if (_toInit) InitCode.Add(instr); else Code.Add(instr); return instr; }

      public int Here { get { return Code.Count; } }

      // Compile a Python-subset expression source; returns an expr-program index, or -1 if
      // the source is empty/unsupported (callers treat -1 as "no usable expression").
      // Identical programs are de-duplicated.
      public int CompileExpr(string src)
      {
         ExprProgram ep = ExprCompiler.Compile(src, this);
         if (ep == null) return -1;
         string key = ep.Key();
         int i;
         if (_exprIntern.TryGetValue(key, out i)) return i;
         i = Exprs.Count;
         Exprs.Add(ep);
         _exprIntern[key] = i;
         return i;
      }

      // Compile the `atl` block of a Show/Scene/Image node into an ATL keyframe program; returns an
      // atl-program index, or -1 if the node has no (usable) ATL. Identical programs are de-duplicated.
      public int CompileAtl(AstNode node)
      {
         if (node == null) return -1;
         AtlProgram ap = AtlCompiler.Compile(node.AtlObj(), Notes);
         if (ap == null) return -1;
         string key = ap.Key();
         int i;
         if (_atlIntern.TryGetValue(key, out i)) return i;
         i = Atls.Count;
         Atls.Add(ap);
         _atlIntern[key] = i;
         return i;
      }

      // Parse a `renpy.imagemap(...)` call source into an imagemap table entry; returns its index
      // or -1 if the source isn't a recognizable imagemap. Identical imagemaps are de-duplicated.
      public int CompileImageMap(string src)
      {
         ImageMapDef d = ImageMapCompiler.Parse(src);
         if (d == null) return -1;
         return InternImageMap(d);
      }

      // Collect every themed layout.imagemap_<screen>(...) call in src into the baked imagemap table.
      // These define whole menu screens (navigation/load_save/preferences/...) rather than a single
      // call site, so they are not emitted as ImageMap ops; the player looks them up by Kind when the
      // corresponding screen is entered.
      public void CollectThemedImageMaps(string src)
      {
         foreach (ImageMapDef d in ImageMapCompiler.ParseThemed(src))
            InternImageMap(d);
      }

      private int InternImageMap(ImageMapDef d)
      {
         string key = d.Key();
         int i;
         if (_imIntern.TryGetValue(key, out i)) return i;
         i = ImageMaps.Count;
         ImageMaps.Add(d);
         _imIntern[key] = i;
         return i;
      }
   }
}
