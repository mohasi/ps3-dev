using System;
using System.Collections.Generic;
using System.Globalization;

namespace ThemeStudio
{
   // reads PSJS source into a tree the runtime can walk. this is a reader for the preview only --
   // Sony's own compiler stays the authority on whether the console will accept a script, and the
   // Validate button still runs it. this exists purely so the preview can move.
   public class PsjsError : Exception
   {
      public PsjsError(string message, int line) : base("line " + line + ": " + message) { }
   }

   internal enum TokenKind { End, Number, Text, Name, Punctuation }

   internal class Token
   {
      public TokenKind Kind;
      public string Spelling = "";
      public double Number;
      public int Line;

      public bool Is(string spelling) { return Spelling == spelling; }
   }

   internal static class PsjsLexer
   {
      // longest first, so "->" is never read as "-" then ">"
      private static readonly string[] Compound = {
         "->", "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "++", "--"
      };

      public static List<Token> Scan(string source)
      {
         var tokens = new List<Token>();
         int at = 0, line = 1;

         while (at < source.Length) {
            char here = source[at];

            // whitespace and both comment styles
            if (here == '\n') { line++; at++; continue; }
            if (char.IsWhiteSpace(here)) { at++; continue; }
            if (here == '/' && at + 1 < source.Length && source[at + 1] == '/') {
               while (at < source.Length && source[at] != '\n') at++;
               continue;
            }
            if (here == '/' && at + 1 < source.Length && source[at + 1] == '*') {
               at += 2;
               while (at + 1 < source.Length && !(source[at] == '*' && source[at + 1] == '/')) {
                  if (source[at] == '\n') line++;
                  at++;
               }
               at = Math.Min(at + 2, source.Length);
               continue;
            }

            if (char.IsDigit(here) || (here == '.' && at + 1 < source.Length && char.IsDigit(source[at + 1]))) {
               int start = at;
               while (at < source.Length && (char.IsDigit(source[at]) || source[at] == '.')) at++;
               if (at < source.Length && (source[at] == 'e' || source[at] == 'E')) {
                  at++;
                  if (at < source.Length && (source[at] == '+' || source[at] == '-')) at++;
                  while (at < source.Length && char.IsDigit(source[at])) at++;
               }
               string spelling = source.Substring(start, at - start);
               double value;
               if (!double.TryParse(spelling, NumberStyles.Float, CultureInfo.InvariantCulture, out value))
                  throw new PsjsError("could not read the number \"" + spelling + "\"", line);
               tokens.Add(new Token { Kind = TokenKind.Number, Spelling = spelling, Number = value, Line = line });
               continue;
            }

            if (here == '"' || here == '\'') {
               char quote = here;
               at++;
               var text = new System.Text.StringBuilder();
               while (at < source.Length && source[at] != quote) {
                  if (source[at] == '\\' && at + 1 < source.Length) { text.Append(unescape(source[++at])); at++; continue; }
                  if (source[at] == '\n') line++;
                  text.Append(source[at++]);
               }
               if (at >= source.Length) throw new PsjsError("a piece of text was never closed", line);
               at++;
               tokens.Add(new Token { Kind = TokenKind.Text, Spelling = text.ToString(), Line = line });
               continue;
            }

            if (char.IsLetter(here) || here == '_' || here == '$') {
               int start = at;
               while (at < source.Length && (char.IsLetterOrDigit(source[at]) || source[at] == '_' || source[at] == '$')) at++;
               tokens.Add(new Token { Kind = TokenKind.Name, Spelling = source.Substring(start, at - start), Line = line });
               continue;
            }

            string found = null;
            foreach (string candidate in Compound)
               if (at + candidate.Length <= source.Length && source.Substring(at, candidate.Length) == candidate) {
                  found = candidate;
                  break;
               }
            if (found == null) found = here.ToString();
            at += found.Length;
            tokens.Add(new Token { Kind = TokenKind.Punctuation, Spelling = found, Line = line });
         }

         tokens.Add(new Token { Kind = TokenKind.End, Spelling = "", Line = line });
         return tokens;
      }

      private static char unescape(char escaped)
      {
         switch (escaped) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            default: return escaped;
         }
      }
   }

   // the tree

   public abstract class PsjsExpression
   {
      public int Line;
      public abstract object Evaluate(PsjsScope scope);
   }

   public abstract class PsjsStatement
   {
      public int Line;
      public abstract void Run(PsjsScope scope);
   }

   public class PsjsProgram
   {
      public readonly List<PsjsStatement> Statements = new List<PsjsStatement>();

      public void Run(PsjsScope scope)
      {
         // functions come into being before anything runs, so a timer set up at the top of a
         // script can name a function written further down
         foreach (PsjsStatement statement in Statements) {
            var declaration = statement as PsjsFunctionDeclaration;
            if (declaration != null) declaration.Run(scope);
         }
         foreach (PsjsStatement statement in Statements)
            if (!(statement is PsjsFunctionDeclaration)) statement.Run(scope);
      }
   }

   internal class PsjsParser
   {
      private readonly List<Token> tokens;
      private int at;

      private PsjsParser(List<Token> tokens) { this.tokens = tokens; }

      public static PsjsProgram Parse(string source)
      {
         var parser = new PsjsParser(PsjsLexer.Scan(source));
         var program = new PsjsProgram();
         while (parser.current.Kind != TokenKind.End) program.Statements.Add(parser.parseStatement());
         return program;
      }

      private Token current { get { return tokens[at]; } }
      private Token take() { return tokens[at++]; }

      private bool skipIf(string spelling)
      {
         if (!current.Is(spelling)) return false;
         at++;
         return true;
      }

      private void expect(string spelling)
      {
         if (!skipIf(spelling)) throw new PsjsError("expected \"" + spelling + "\" but found \"" + current.Spelling + "\"", current.Line);
      }

      private void endStatement() { skipIf(";"); }

      // statements

      private PsjsStatement parseStatement()
      {
         int line = current.Line;

         if (skipIf(";")) return new PsjsBlock { Line = line };
         if (current.Is("{")) return parseBlock();
         if (current.Is("var")) return parseVar();
         if (current.Is("function")) return parseFunctionDeclaration();
         if (current.Is("if")) return parseIf();
         if (current.Is("while")) return parseWhile();
         if (current.Is("for")) return parseFor();
         if (current.Is("return")) {
            take();
            PsjsExpression value = current.Is(";") || current.Is("}") ? null : parseExpression();
            endStatement();
            return new PsjsReturnStatement { Value = value, Line = line };
         }
         if (current.Is("break")) { take(); endStatement(); return new PsjsBreakStatement { Line = line }; }
         if (current.Is("continue")) { take(); endStatement(); return new PsjsContinueStatement { Line = line }; }

         var statement = new PsjsExpressionStatement { Value = parseExpression(), Line = line };
         endStatement();
         return statement;
      }

      private PsjsBlock parseBlock()
      {
         var block = new PsjsBlock { Line = current.Line };
         expect("{");
         while (!current.Is("}") && current.Kind != TokenKind.End) block.Statements.Add(parseStatement());
         expect("}");
         return block;
      }

      private PsjsStatement parseVar()
      {
         var declaration = new PsjsVarStatement { Line = current.Line };
         take();
         do {
            string name = takeName();
            PsjsExpression initial = skipIf("=") ? parseAssignment() : null;
            declaration.Names.Add(name);
            declaration.Initials.Add(initial);
         } while (skipIf(","));
         endStatement();
         return declaration;
      }

      private PsjsStatement parseFunctionDeclaration()
      {
         int line = current.Line;
         take();
         string name = takeName();
         return new PsjsFunctionDeclaration { Name = name, Body = parseFunctionRest(name), Line = line };
      }

      private PsjsFunctionLiteral parseFunctionRest(string name)
      {
         var literal = new PsjsFunctionLiteral { Name = name, Line = current.Line };
         expect("(");
         while (!current.Is(")")) {
            literal.Parameters.Add(takeName());
            if (!skipIf(",")) break;
         }
         expect(")");
         literal.Body = parseBlock();
         return literal;
      }

      private PsjsStatement parseIf()
      {
         var statement = new PsjsIfStatement { Line = current.Line };
         take();
         expect("(");
         statement.Condition = parseExpression();
         expect(")");
         statement.WhenTrue = parseStatement();
         if (skipIf("else")) statement.WhenFalse = parseStatement();
         return statement;
      }

      private PsjsStatement parseWhile()
      {
         var statement = new PsjsWhileStatement { Line = current.Line };
         take();
         expect("(");
         statement.Condition = parseExpression();
         expect(")");
         statement.Body = parseStatement();
         return statement;
      }

      private PsjsStatement parseFor()
      {
         var statement = new PsjsForStatement { Line = current.Line };
         take();
         expect("(");
         // parseVar eats its own ";"; a bare expression initialiser has to have it taken here,
         // or every part of the loop after it is read one slot too early
         if (current.Is(";")) take();
         else if (current.Is("var")) statement.Setup = parseVar();
         else {
            statement.Setup = new PsjsExpressionStatement { Value = parseExpression(), Line = current.Line };
            expect(";");
         }
         if (!current.Is(";")) statement.Condition = parseExpression();
         expect(";");
         if (!current.Is(")")) statement.Step = parseExpression();
         expect(")");
         statement.Body = parseStatement();
         return statement;
      }

      private string takeName()
      {
         Token token = take();
         if (token.Kind != TokenKind.Name) throw new PsjsError("expected a name, found \"" + token.Spelling + "\"", token.Line);
         return token.Spelling;
      }

      // expressions, loosest binding first

      private PsjsExpression parseExpression() { return parseAssignment(); }

      private PsjsExpression parseAssignment()
      {
         PsjsExpression left = parseTernary();
         if (current.Kind != TokenKind.Punctuation) return left;

         string op = current.Spelling;
         if (op != "=" && op != "+=" && op != "-=" && op != "*=" && op != "/=") return left;
         int line = take().Line;
         return new PsjsAssignment { Operator = op, Target = left, Value = parseAssignment(), Line = line };
      }

      // condition ? whenTrue : whenFalse
      private PsjsExpression parseTernary()
      {
         PsjsExpression condition = parseOr();
         if (!current.Is("?")) return condition;
         int line = take().Line;
         PsjsExpression whenTrue = parseAssignment();
         expect(":");
         PsjsExpression whenFalse = parseAssignment();
         return new PsjsTernary { Condition = condition, WhenTrue = whenTrue, WhenFalse = whenFalse, Line = line };
      }

      private PsjsExpression parseOr() { return parseBinary(0); }

      private static readonly string[][] BinaryLevels = {
         new[] { "||" }, new[] { "&&" }, new[] { "==", "!=" },
         new[] { "<", ">", "<=", ">=" }, new[] { "+", "-" }, new[] { "*", "/", "%" }
      };

      // a deeply nested expression recurses once per level, and a stack overflow cannot be caught
      // in .NET -- it ends the process. no real script nests anywhere near this far.
      private const int MostNesting = 400;
      private int nesting;

      private PsjsExpression parseBinary(int level)
      {
         if (level >= BinaryLevels.Length) return parseUnary();
         PsjsExpression left = parseBinary(level + 1);
         while (current.Kind == TokenKind.Punctuation && Array.IndexOf(BinaryLevels[level], current.Spelling) >= 0) {
            string op = current.Spelling;
            int line = take().Line;
            left = new PsjsBinary { Operator = op, Left = left, Right = parseBinary(level + 1), Line = line };
         }
         return left;
      }

      private PsjsExpression parseUnary()
      {
         if (++nesting > MostNesting) throw new PsjsError("this expression is nested far too deeply", current.Line);
         try {
            return parseUnaryInner();
         } finally {
            nesting--;
         }
      }

      private PsjsExpression parseUnaryInner()
      {
         if (current.Is("-") || current.Is("!") || current.Is("+")) {
            string op = current.Spelling;
            int line = take().Line;
            return new PsjsUnary { Operator = op, Operand = parseUnary(), Line = line };
         }
         // ++i and --i. the value they produce is the same either way here, because nothing in a
         // theme script reads the result of a step -- it is written for its effect.
         if (current.Is("++") || current.Is("--")) {
            string op = current.Spelling;
            int line = take().Line;
            return new PsjsStep { Operator = op, Target = parseUnary(), Line = line };
         }
         if (current.Is("new")) {
            int line = take().Line;
            PsjsExpression callee = parseCallChain(parsePrimary(), false);
            var arguments = new List<PsjsExpression>();
            if (current.Is("(")) arguments = parseArguments();
            return parseCallChain(new PsjsCall { Callee = callee, Arguments = arguments, Line = line }, true);
         }
         return parseCallChain(parsePrimary(), true);
      }

      // .name  ->name  [index]  (arguments)
      private PsjsExpression parseCallChain(PsjsExpression target, bool allowCalls)
      {
         while (true) {
            if (current.Is(".") || current.Is("->")) {
               int line = take().Line;
               target = new PsjsMember { Target = target, Name = takeName(), Line = line };
            } else if (current.Is("[")) {
               int line = take().Line;
               PsjsExpression index = parseExpression();
               expect("]");
               target = new PsjsIndex { Target = target, Index = index, Line = line };
            } else if (allowCalls && current.Is("(")) {
               int line = current.Line;
               target = new PsjsCall { Callee = target, Arguments = parseArguments(), Line = line };
            } else if (current.Is("++") || current.Is("--")) {
               string op = current.Spelling;
               int line = take().Line;
               target = new PsjsStep { Operator = op, Target = target, Line = line };
            } else {
               return target;
            }
         }
      }

      private List<PsjsExpression> parseArguments()
      {
         var arguments = new List<PsjsExpression>();
         expect("(");
         while (!current.Is(")")) {
            arguments.Add(parseAssignment());
            if (!skipIf(",")) break;
         }
         expect(")");
         return arguments;
      }

      private PsjsExpression parsePrimary()
      {
         Token token = current;

         if (token.Kind == TokenKind.Number) { take(); return new PsjsLiteral { Value = token.Number, Line = token.Line }; }
         if (token.Kind == TokenKind.Text) { take(); return new PsjsLiteral { Value = token.Spelling, Line = token.Line }; }

         if (token.Is("(")) {
            take();
            PsjsExpression inner = parseExpression();
            expect(")");
            return inner;
         }

         if (token.Is("[")) {
            var list = new PsjsArrayLiteral { Line = take().Line };
            while (!current.Is("]") && current.Kind != TokenKind.End) {
               list.Items.Add(parseAssignment());
               if (!skipIf(",")) break;
            }
            expect("]");
            return list;
         }

         // a vector literal. components are read without comparisons so the closing ">" is never
         // mistaken for "greater than" -- no real script compares inside a vector literal.
         if (token.Is("<")) {
            var vector = new PsjsVectorLiteral { Line = take().Line };
            while (!current.Is(">")) {
               vector.Components.Add(parseBinary(4));
               if (!skipIf(",")) break;
            }
            expect(">");
            return vector;
         }

         if (token.Kind == TokenKind.Name) {
            take();
            if (token.Is("true")) return new PsjsLiteral { Value = true, Line = token.Line };
            if (token.Is("false")) return new PsjsLiteral { Value = false, Line = token.Line };
            if (token.Is("null") || token.Is("undefined")) return new PsjsLiteral { Value = null, Line = token.Line };
            if (token.Is("function")) return parseFunctionRest("");
            return new PsjsName { Name = token.Spelling, Line = token.Line };
         }

         throw new PsjsError("did not expect \"" + token.Spelling + "\" here", token.Line);
      }
   }
}
