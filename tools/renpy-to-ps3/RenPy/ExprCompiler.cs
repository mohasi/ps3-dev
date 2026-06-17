using System.Collections.Generic;
using System.Globalization;

namespace RenpyToPs3.RenPy
{
   // Expression opcodes for the on-device RPN evaluator. MUST stay in lockstep with the C
   // `ExprOp` enum in expr.h. A compiled expression is a postfix (RPN) list of these; the
   // player runs a tiny stack machine over it. Operand `Arg` meaning depends on the op:
   //   PushInt  -> the integer literal value
   //   PushBool -> 0 or 1
   //   PushNone -> (unused)
   //   PushFloat-> string-table id of the float literal's text (player does atof)
   //   PushStr  -> string-table id of the string literal
   //   LoadVar  -> string-table id of the (possibly dotted) variable name
   //   all operators -> (unused, -1)
   public enum ExprOp : byte
   {
      PushInt = 0, PushBool, PushNone, PushFloat, PushStr, LoadVar,
      Neg, Not,
      Add, Sub, Mul, Div, Mod,
      Eq, Ne, Lt, Le, Gt, Ge,
      And, Or,
      FloorDiv,  // `//` : always floors (Python2 `/` on ints already floors; this also floors floats)
      Max, Min   // built-in max()/min(): emitted as a left-fold of binary ops (max(a,b,c)=max(max(a,b),c))
   }

   public sealed class ExprInstr
   {
      public ExprOp Op;
      public int Arg;
      public ExprInstr(ExprOp op, int arg) { Op = op; Arg = arg; }
   }

   public sealed class ExprProgram
   {
      public readonly List<ExprInstr> Ops = new List<ExprInstr>();
      public void Emit(ExprOp op, int arg) { Ops.Add(new ExprInstr(op, arg)); }

      // Structural key so identical expressions can be de-duplicated by the IR.
      public string Key()
      {
         System.Text.StringBuilder sb = new System.Text.StringBuilder();
         foreach (ExprInstr i in Ops) { sb.Append((int)i.Op); sb.Append(':'); sb.Append(i.Arg); sb.Append(';'); }
         return sb.ToString();
      }
   }

   // Compiles a Python-subset expression string into an ExprProgram (RPN). Strings/float
   // literals/var names are interned into the shared IR string table (so the player has a
   // single string pool). Returns null for anything outside the supported subset -- callers
   // treat that as "unsupported" (conditions fall back to a safe default, assignments are
   // dropped) and the construct is recorded in the convert report. Supported:
   //   literals: int, float, 'str'/"str", True, False, None
   //   names:    foo, foo.bar.baz  (dotted treated as one lookup key)
   //   unary:    -x, not x
   //   binary:   + - * / // %   ==  !=  <  <=  >  >=   and  or
   // Unsupported (-> null): function calls, subscripts/slices, **, `in`, `is`, lists/dicts,
   // string concatenation with non-literals is fine (handled by + at runtime), ternary, etc.
   public sealed class ExprCompiler
   {
      private readonly string _s;
      private int _pos;
      private readonly IrProgram _ir;
      private bool _bad;

      private ExprCompiler(string s, IrProgram ir) { _s = s ?? ""; _ir = ir; }

      public static ExprProgram Compile(string src, IrProgram ir)
      {
         if (string.IsNullOrEmpty(src)) return null;
         string t = src.Trim();
         if (t.Length == 0) return null;
         ExprCompiler c = new ExprCompiler(t, ir);
         ExprProgram p = new ExprProgram();
         c.ParseOr(p);
         c.SkipWs();
         if (c._bad || c._pos < c._s.Length) return null;   // leftover tokens => unsupported
         return p.Ops.Count > 0 ? p : null;
      }

      // ---- lexer helpers ----
      private void SkipWs() { while (_pos < _s.Length && (_s[_pos] == ' ' || _s[_pos] == '\t' || _s[_pos] == '\r' || _s[_pos] == '\n')) _pos++; }
      private char Peek() { return _pos < _s.Length ? _s[_pos] : '\0'; }
      private char PeekAt(int k) { return (_pos + k) < _s.Length ? _s[_pos + k] : '\0'; }

      // Consumes a keyword/operator-word if it matches exactly at a word boundary.
      private bool MatchWord(string w)
      {
         SkipWs();
         if (_pos + w.Length > _s.Length) return false;
         for (int i = 0; i < w.Length; i++) if (_s[_pos + i] != w[i]) return false;
         char after = (_pos + w.Length < _s.Length) ? _s[_pos + w.Length] : '\0';
         if (IsIdentChar(after)) return false;               // e.g. "android" must not match "and"
         _pos += w.Length;
         return true;
      }

      private bool MatchSym(string sym)
      {
         SkipWs();
         if (_pos + sym.Length > _s.Length) return false;
         for (int i = 0; i < sym.Length; i++) if (_s[_pos + i] != sym[i]) return false;
         _pos += sym.Length;
         return true;
      }

      private static bool IsIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
      private static bool IsIdentChar(char c) { return IsIdentStart(c) || (c >= '0' && c <= '9') || c == '.'; }

      // ---- recursive-descent (precedence climbing), emitting postfix ----
      // or  ->  and ('or' and)*
      private void ParseOr(ExprProgram p)
      {
         ParseAnd(p);
         while (!_bad && MatchWord("or")) { ParseAnd(p); p.Emit(ExprOp.Or, -1); }
      }

      private void ParseAnd(ExprProgram p)
      {
         ParseNot(p);
         while (!_bad && MatchWord("and")) { ParseNot(p); p.Emit(ExprOp.And, -1); }
      }

      private void ParseNot(ExprProgram p)
      {
         if (MatchWord("not")) { ParseNot(p); p.Emit(ExprOp.Not, -1); return; }
         ParseCmp(p);
      }

      private void ParseCmp(ExprProgram p)
      {
         ParseAdd(p);
         while (!_bad)
         {
            ExprOp op;
            // reject unsupported comparison words up front
            SkipWs();
            if (MatchWord("in") || MatchWord("is")) { _bad = true; return; }
            if (MatchSym("==")) op = ExprOp.Eq;
            else if (MatchSym("!=")) op = ExprOp.Ne;
            else if (MatchSym("<=")) op = ExprOp.Le;
            else if (MatchSym(">=")) op = ExprOp.Ge;
            else if (Peek() == '<' && PeekAt(1) != '<') { _pos++; op = ExprOp.Lt; }
            else if (Peek() == '>' && PeekAt(1) != '>') { _pos++; op = ExprOp.Gt; }
            else break;
            ParseAdd(p);
            p.Emit(op, -1);
         }
      }

      private void ParseAdd(ExprProgram p)
      {
         ParseMul(p);
         while (!_bad)
         {
            SkipWs();
            if (Peek() == '+') { _pos++; ParseMul(p); p.Emit(ExprOp.Add, -1); }
            else if (Peek() == '-') { _pos++; ParseMul(p); p.Emit(ExprOp.Sub, -1); }
            else break;
         }
      }

      private void ParseMul(ExprProgram p)
      {
         ParseUnary(p);
         while (!_bad)
         {
            SkipWs();
            if (Peek() == '*' && PeekAt(1) == '*') { _bad = true; return; }   // ** unsupported
            if (Peek() == '*') { _pos++; ParseUnary(p); p.Emit(ExprOp.Mul, -1); }
            else if (Peek() == '/' && PeekAt(1) == '/') { _pos += 2; ParseUnary(p); p.Emit(ExprOp.FloorDiv, -1); }  // `//` floor division
            else if (Peek() == '/') { _pos++; ParseUnary(p); p.Emit(ExprOp.Div, -1); }
            else if (Peek() == '%') { _pos++; ParseUnary(p); p.Emit(ExprOp.Mod, -1); }
            else break;
         }
      }

      private void ParseUnary(ExprProgram p)
      {
         SkipWs();
         if (Peek() == '-') { _pos++; ParseUnary(p); p.Emit(ExprOp.Neg, -1); return; }
         if (Peek() == '+') { _pos++; ParseUnary(p); return; }   // unary plus: no-op
         ParseAtom(p);
      }

      private void ParseAtom(ExprProgram p)
      {
         SkipWs();
         char c = Peek();
         if (c == '\0') { _bad = true; return; }

         if (c == '(')
         {
            _pos++;
            ParseOr(p);
            SkipWs();
            if (Peek() != ')') { _bad = true; return; }
            _pos++;
            RejectTrailer();
            return;
         }

         if (c == '\'' || c == '"') { ParseString(p, c); RejectTrailer(); return; }

         if (c >= '0' && c <= '9') { ParseNumber(p); RejectTrailer(); return; }
         if (c == '.' && PeekAt(1) >= '0' && PeekAt(1) <= '9') { ParseNumber(p); RejectTrailer(); return; }

         if (IsIdentStart(c)) { ParseNameOrKeyword(p); return; }

         _bad = true;
      }

      // After an atom, a '(' (call) or '[' (subscript) means an unsupported construct.
      private void RejectTrailer()
      {
         SkipWs();
         if (Peek() == '(' || Peek() == '[') _bad = true;
      }

      private void ParseString(ExprProgram p, char quote)
      {
         _pos++;   // opening quote
         System.Text.StringBuilder sb = new System.Text.StringBuilder();
         while (_pos < _s.Length)
         {
            char c = _s[_pos++];
            if (c == '\\' && _pos < _s.Length)
            {
               char e = _s[_pos++];
               switch (e) { case 'n': sb.Append('\n'); break; case 't': sb.Append('\t'); break; default: sb.Append(e); break; }
               continue;
            }
            if (c == quote) { p.Emit(ExprOp.PushStr, _ir.Intern(sb.ToString())); return; }
            sb.Append(c);
         }
         _bad = true;   // unterminated string
      }

      private void ParseNumber(ExprProgram p)
      {
         int start = _pos;
         bool isFloat = false;
         while (_pos < _s.Length)
         {
            char c = _s[_pos];
            if (c >= '0' && c <= '9') { _pos++; }
            else if (c == '.') { isFloat = true; _pos++; }
            else if (c == 'e' || c == 'E') { isFloat = true; _pos++; if (Peek() == '+' || Peek() == '-') _pos++; }
            else break;
         }
         string tok = _s.Substring(start, _pos - start);
         if (isFloat)
         {
            double dv;
            if (!double.TryParse(tok, NumberStyles.Float, CultureInfo.InvariantCulture, out dv)) { _bad = true; return; }
            p.Emit(ExprOp.PushFloat, _ir.Intern(tok));
         }
         else
         {
            long lv;
            if (!long.TryParse(tok, NumberStyles.Integer, CultureInfo.InvariantCulture, out lv)) { _bad = true; return; }
            p.Emit(ExprOp.PushInt, (int)lv);
         }
      }

      private void ParseNameOrKeyword(ExprProgram p)
      {
         int start = _pos;
         while (_pos < _s.Length && IsIdentChar(_s[_pos])) _pos++;
         string name = _s.Substring(start, _pos - start);

         // Built-in max()/min(): the only supported calls. Compiled as a left-fold of binary ops
         // (max(a,b,c) -> a b Max c Max), matching Python's reduction + "first arg wins on ties".
         if (name == "max" || name == "min")
         {
            SkipWs();
            if (Peek() == '(')
            {
               _pos++;                                       // consume '('
               ExprOp fold = (name == "max") ? ExprOp.Max : ExprOp.Min;
               ParseOr(p);                                   // first argument
               int args = 1;
               while (!_bad && MatchSym(",")) { ParseOr(p); p.Emit(fold, -1); args++; }
               SkipWs();
               if (_bad || args < 2 || Peek() != ')') { _bad = true; return; }
               _pos++;                                       // consume ')'
               RejectTrailer();
               return;
            }
            // not a call -> fall through and treat "max"/"min" as an ordinary variable name
         }

         if (name == "True") { p.Emit(ExprOp.PushBool, 1); RejectTrailer(); return; }
         if (name == "False") { p.Emit(ExprOp.PushBool, 0); RejectTrailer(); return; }
         if (name == "None") { p.Emit(ExprOp.PushNone, -1); RejectTrailer(); return; }
         // bare operator-words here would be a syntax error in atom position
         if (name == "and" || name == "or" || name == "not" || name == "in" || name == "is") { _bad = true; return; }

         p.Emit(ExprOp.LoadVar, _ir.Intern(name));
         RejectTrailer();   // foo(...) / foo[...] unsupported
      }
   }
}
