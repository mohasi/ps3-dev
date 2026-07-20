using System;
using System.Collections.Generic;
using System.Globalization;

namespace ThemeStudio
{
   public interface IPsjsMembers
   {
      object GetMember(string name);
      void SetMember(string name, object value);
   }

   public interface IPsjsIndexed
   {
      object GetIndex(int index);
      void SetIndex(int index, object value);
   }

   public interface IPsjsCallable
   {
      object Call(object[] arguments);
   }

   // PSJS's own four-component vector, written <x, y, z> or <x, y, z, w> and read with ->
   public class PsjsVector
   {
      public double X, Y, Z, W;

      public PsjsVector() { }
      public PsjsVector(double x, double y, double z, double w) { X = x; Y = y; Z = z; W = w; }
      public PsjsVector(Vec3 value, double w) : this(value.X, value.Y, value.Z, w) { }

      public double this[int index]
      {
         get { return index == 0 ? X : index == 1 ? Y : index == 2 ? Z : W; }
         set {
            if (index == 0) X = value; else if (index == 1) Y = value;
            else if (index == 2) Z = value; else W = value;
         }
      }

      public PsjsVector Copy() { return new PsjsVector(X, Y, Z, W); }
      public Vec3 ToVec3() { return new Vec3(X, Y, Z); }

      public PsjsVector Scaled(double factor) { return new PsjsVector(X * factor, Y * factor, Z * factor, W * factor); }

      public static PsjsVector Mix(PsjsVector from, PsjsVector to, double fraction)
      {
         return new PsjsVector(from.X + (to.X - from.X) * fraction, from.Y + (to.Y - from.Y) * fraction,
                               from.Z + (to.Z - from.Z) * fraction, from.W + (to.W - from.W) * fraction);
      }

      public double GetComponent(string name, int line)
      {
         int index = indexOf(name);
         if (index < 0) throw new PsjsError("a vector has no part called \"" + name + "\"", line);
         return this[index];
      }

      public void SetComponent(string name, double value, int line)
      {
         int index = indexOf(name);
         if (index < 0) throw new PsjsError("a vector has no part called \"" + name + "\"", line);
         this[index] = value;
      }

      // colours are written with the same four slots, so r/g/b/a name them too
      private static int indexOf(string name)
      {
         switch (name) {
            case "x": case "r": return 0;
            case "y": case "g": return 1;
            case "z": case "b": return 2;
            case "w": case "a": return 3;
            default: return -1;
         }
      }
   }

   // what one frame of script may cost. a function that calls itself overflows the stack, which
   // .NET cannot catch -- it kills the process and any unsaved work with it. the step ceiling is
   // shared across loops because a per-loop count resets every time an inner loop starts over.
   public static class PsjsBudget
   {
      private const int MostSteps = 2000000;   // loop turns in one frame, across every loop
      private const int MostDepth = 200;       // how deep calls may nest

      private static int steps, depth;

      public static void Begin() { steps = 0; depth = 0; }

      public static void CountStep(int line)
      {
         if (++steps > MostSteps) throw new PsjsError("this script is still going round after a very long time", line);
      }

      public static void Enter(int line)
      {
         if (++depth > MostDepth) { depth--; throw new PsjsError("this calls itself too many times over", line); }
      }

      public static void Leave() { depth--; }
   }

   public static class PsjsValues
   {
      private const int MostLoopTurns = 100000;   // a runaway loop must not hang the editor

      public static double ToNumber(object value)
      {
         if (value is double) return (double)value;
         if (value is bool) return (bool)value ? 1 : 0;
         var text = value as string;
         double parsed;
         if (text != null && double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out parsed))
            return parsed;
         return 0;
      }

      public static bool ToBool(object value)
      {
         if (value == null) return false;
         if (value is bool) return (bool)value;
         if (value is double) return (double)value != 0;
         var text = value as string;
         return text == null || text.Length > 0;
      }

      public static string ToText(object value)
      {
         if (value == null) return "null";
         if (value is bool) return (bool)value ? "true" : "false";
         if (value is double) return ((double)value).ToString("0.####", CultureInfo.InvariantCulture);
         var vector = value as PsjsVector;
         if (vector != null)
            return "<" + ToText(vector.X) + ", " + ToText(vector.Y) + ", " + ToText(vector.Z) + ">";
         return value.ToString();
      }

      public static object Combine(string op, object left, object right, int line)
      {
         var leftVector = left as PsjsVector;
         var rightVector = right as PsjsVector;

         // two vectors work part by part; a vector and a number scales. multiplying a colour by
         // <1,1,1,0> for a see-through copy is the idiom Sony's own sample script opens with.
         if (leftVector != null || rightVector != null) {
            if (leftVector != null && rightVector != null) {
               switch (op) {
                  case "+": return eachPart(leftVector, rightVector, (a1, b1) => a1 + b1);
                  case "-": return eachPart(leftVector, rightVector, (a1, b1) => a1 - b1);
                  case "*": return eachPart(leftVector, rightVector, (a1, b1) => a1 * b1);
                  case "/": return eachPart(leftVector, rightVector, (a1, b1) => b1 == 0 ? 0 : a1 / b1);
               }
               throw new PsjsError("cannot use \"" + op + "\" on two vectors", line);
            }
            if (op == "*" || op == "/") {
               PsjsVector vector = leftVector ?? rightVector;
               double factor = ToNumber(leftVector != null ? right : left);
               return vector.Scaled(op == "*" ? factor : (factor == 0 ? 0 : 1 / factor));
            }
            throw new PsjsError("cannot use \"" + op + "\" on a vector like that", line);
         }

         if (op == "+" && (left is string || right is string)) return ToText(left) + ToText(right);

         // as strings: read as numbers, any two words are both zero and so compare equal
         if ((op == "==" || op == "!=") && left is string && right is string)
            return op == "==" ? (string)left == (string)right : (string)left != (string)right;

         double a = ToNumber(left), b = ToNumber(right);
         switch (op) {
            case "+": return a + b;
            case "-": return a - b;
            case "*": return a * b;
            case "/": return b == 0 ? 0 : a / b;
            case "%": return b == 0 ? 0 : a % b;
            case "<": return a < b;
            case ">": return a > b;
            case "<=": return a <= b;
            case ">=": return a >= b;
            case "==": return a == b;
            case "!=": return a != b;
            default: throw new PsjsError("unknown operator \"" + op + "\"", line);
         }
      }

      private static PsjsVector eachPart(PsjsVector left, PsjsVector right, Func<double, double, double> combine)
      {
         return new PsjsVector(combine(left.X, right.X), combine(left.Y, right.Y),
                               combine(left.Z, right.Z), combine(left.W, right.W));
      }

      public static void GuardLoop(int turns, int line)
      {
         if (turns > MostLoopTurns) throw new PsjsError("a loop here never finishes", line);
         PsjsBudget.CountStep(line);
      }
   }

   // names in scope. a lookup walks outwards; an assignment to an undeclared name lands globally,
   // which is what plain javascript does and what real scripts rely on.
   public class PsjsScope
   {
      private readonly Dictionary<string, object> values = new Dictionary<string, object>();
      private readonly PsjsScope parent;

      public PsjsScope(PsjsScope parent) { this.parent = parent; }

      public object Get(string name, int line)
      {
         for (PsjsScope scope = this; scope != null; scope = scope.parent) {
            object value;
            if (scope.values.TryGetValue(name, out value)) return value;
         }
         throw new PsjsError("nothing here is called \"" + name + "\"", line);
      }

      public void Declare(string name, object value) { values[name] = value; }

      public void Set(string name, object value)
      {
         for (PsjsScope scope = this; scope != null; scope = scope.parent)
            if (scope.values.ContainsKey(name)) { scope.values[name] = value; return; }
         root.values[name] = value;
      }

      private PsjsScope root
      {
         get {
            PsjsScope scope = this;
            while (scope.parent != null) scope = scope.parent;
            return scope;
         }
      }
   }

   // a function written in the script
   public class PsjsFunction : IPsjsCallable
   {
      private readonly PsjsFunctionLiteral literal;
      private readonly PsjsScope captured;

      public PsjsFunction(PsjsFunctionLiteral literal, PsjsScope captured)
      {
         this.literal = literal;
         this.captured = captured;
      }

      public object Call(object[] arguments)
      {
         var inner = new PsjsScope(captured);
         for (int index = 0; index < literal.Parameters.Count; index++)
            inner.Declare(literal.Parameters[index], index < arguments.Length ? arguments[index] : null);

         PsjsBudget.Enter(literal.Line);
         try {
            literal.Body.Run(inner);
         } catch (PsjsReturnSignal signal) {
            return signal.Value;
         } finally {
            PsjsBudget.Leave();
         }
         return null;
      }
   }

   // anything the editor supplies rather than the script: Math.sin, an actor's setPosition, ...
   public class PsjsNative : IPsjsCallable
   {
      private readonly Func<object[], object> body;
      public PsjsNative(Func<object[], object> body) { this.body = body; }
      public object Call(object[] arguments) { return body(arguments); }
   }

   // a script's array. Sony's own samples open with "new Array(n)" and index it.
   public class PsjsArray : IPsjsIndexed, IPsjsMembers
   {
      private readonly List<object> items = new List<object>();

      public PsjsArray() { }

      public PsjsArray(int length)
      {
         if (length < 0 || length > MostItems)
            throw new PsjsError("an array of " + length + " is not a size a theme script can use", 0);
         while (items.Count < length) items.Add(null);
      }

      public void Push(object value) { items.Add(value); }

      public object GetIndex(int index)
      {
         return index >= 0 && index < items.Count ? items[index] : null;
      }

      // writing past the end grows it, as a script expects
      public void SetIndex(int index, object value)
      {
         if (index < 0 || index > MostItems) return;
         while (items.Count <= index) items.Add(null);
         items[index] = value;
      }

      public object GetMember(string name) { return name == "length" ? (object)(double)items.Count : null; }

      public void SetMember(string name, object value) { }

      private const int MostItems = 100000;
   }

   // a plain bag of named values, used for Math and System
   public class PsjsBag : IPsjsMembers
   {
      private readonly Dictionary<string, object> members = new Dictionary<string, object>();

      public void Add(string name, object value) { members[name] = value; }

      public void AddFunction(string name, Func<object[], object> body) { members[name] = new PsjsNative(body); }

      public object GetMember(string name)
      {
         object value;
         return members.TryGetValue(name, out value) ? value : null;
      }

      public void SetMember(string name, object value) { members[name] = value; }
   }
}
