using System;
using System.Collections.Generic;

namespace ThemeStudio
{
   // what each piece of the tree does when it runs. kept beside the parser rather than folded into
   // it so the shape of the language stays readable on its own.

   public class PsjsLiteral : PsjsExpression
   {
      public object Value;
      public override object Evaluate(PsjsScope scope) { return Value; }
   }

   public class PsjsName : PsjsExpression
   {
      public string Name;
      public override object Evaluate(PsjsScope scope) { return scope.Get(Name, Line); }
   }

   public class PsjsVectorLiteral : PsjsExpression
   {
      public readonly List<PsjsExpression> Components = new List<PsjsExpression>();

      public override object Evaluate(PsjsScope scope)
      {
         var vector = new PsjsVector();
         for (int index = 0; index < Components.Count && index < 4; index++)
            vector[index] = PsjsValues.ToNumber(Components[index].Evaluate(scope));
         if (Components.Count < 4) vector.W = 1.0;   // a colour written <r,g,b> is fully opaque
         return vector;
      }
   }

   public class PsjsMember : PsjsExpression
   {
      public PsjsExpression Target;
      public string Name;

      public override object Evaluate(PsjsScope scope)
      {
         object target = Target.Evaluate(scope);
         var vector = target as PsjsVector;
         if (vector != null) return vector.GetComponent(Name, Line);

         var members = target as IPsjsMembers;
         if (members == null) throw new PsjsError("\"" + Name + "\" was asked of something that has no parts", Line);
         return members.GetMember(Name);
      }

      public void Assign(PsjsScope scope, object value)
      {
         object target = Target.Evaluate(scope);
         var vector = target as PsjsVector;
         if (vector != null) { vector.SetComponent(Name, PsjsValues.ToNumber(value), Line); return; }

         var members = target as IPsjsMembers;
         if (members == null) throw new PsjsError("cannot set \"" + Name + "\" on that", Line);
         members.SetMember(Name, value);
      }
   }

   public class PsjsIndex : PsjsExpression
   {
      public PsjsExpression Target;
      public PsjsExpression Index;

      public override object Evaluate(PsjsScope scope)
      {
         var indexed = Target.Evaluate(scope) as IPsjsIndexed;
         if (indexed == null) throw new PsjsError("that cannot be indexed", Line);
         return indexed.GetIndex((int)PsjsValues.ToNumber(Index.Evaluate(scope)));
      }

      public void Assign(PsjsScope scope, object value)
      {
         var indexed = Target.Evaluate(scope) as IPsjsIndexed;
         if (indexed == null) throw new PsjsError("that cannot be indexed", Line);
         indexed.SetIndex((int)PsjsValues.ToNumber(Index.Evaluate(scope)), value);
      }
   }

   // "new Thing(...)" and "thing(...)" are the same call here: everything constructible in PSJS is
   // built in, and each built-in knows whether it hands back a fresh value or an existing handle.
   public class PsjsCall : PsjsExpression
   {
      public PsjsExpression Callee;
      public List<PsjsExpression> Arguments = new List<PsjsExpression>();

      public override object Evaluate(PsjsScope scope)
      {
         var callable = Callee.Evaluate(scope) as IPsjsCallable;
         if (callable == null) throw new PsjsError("that is not something that can be called", Line);

         var values = new object[Arguments.Count];
         for (int index = 0; index < values.Length; index++) values[index] = Arguments[index].Evaluate(scope);
         return callable.Call(values);
      }
   }

   public class PsjsUnary : PsjsExpression
   {
      public string Operator;
      public PsjsExpression Operand;

      public override object Evaluate(PsjsScope scope)
      {
         object value = Operand.Evaluate(scope);
         switch (Operator) {
            case "-":
               var vector = value as PsjsVector;
               return vector != null ? (object)vector.Scaled(-1) : -PsjsValues.ToNumber(value);
            case "!": return !PsjsValues.ToBool(value);
            default: return PsjsValues.ToNumber(value);
         }
      }
   }

   public class PsjsBinary : PsjsExpression
   {
      public string Operator;
      public PsjsExpression Left;
      public PsjsExpression Right;

      public override object Evaluate(PsjsScope scope)
      {
         if (Operator == "&&") return PsjsValues.ToBool(Left.Evaluate(scope)) ? Right.Evaluate(scope) : false;
         if (Operator == "||") {
            object left = Left.Evaluate(scope);
            return PsjsValues.ToBool(left) ? left : Right.Evaluate(scope);
         }
         return PsjsValues.Combine(Operator, Left.Evaluate(scope), Right.Evaluate(scope), Line);
      }
   }

   public class PsjsAssignment : PsjsExpression
   {
      public string Operator;
      public PsjsExpression Target;
      public PsjsExpression Value;

      public override object Evaluate(PsjsScope scope)
      {
         object value = Value.Evaluate(scope);
         if (Operator != "=")
            value = PsjsValues.Combine(Operator.Substring(0, 1), Target.Evaluate(scope), value, Line);

         Store(Target, value, scope, Line);
         return value;
      }

      // shared with ++ and --, which are assignments written the short way
      public static void Store(PsjsExpression target, object value, PsjsScope scope, int line)
      {
         var name = target as PsjsName;
         if (name != null) { scope.Set(name.Name, value); return; }

         var member = target as PsjsMember;
         if (member != null) { member.Assign(scope, value); return; }

         var index = target as PsjsIndex;
         if (index != null) { index.Assign(scope, value); return; }

         throw new PsjsError("that cannot be assigned to", line);
      }
   }

   public class PsjsFunctionLiteral : PsjsExpression
   {
      public string Name = "";
      public readonly List<string> Parameters = new List<string>();
      public PsjsBlock Body;

      public override object Evaluate(PsjsScope scope) { return new PsjsFunction(this, scope); }
   }

   // statements

   public class PsjsBlock : PsjsStatement
   {
      public readonly List<PsjsStatement> Statements = new List<PsjsStatement>();

      public override void Run(PsjsScope scope)
      {
         foreach (PsjsStatement statement in Statements) statement.Run(scope);
      }
   }

   public class PsjsExpressionStatement : PsjsStatement
   {
      public PsjsExpression Value;
      public override void Run(PsjsScope scope) { Value.Evaluate(scope); }
   }

   public class PsjsVarStatement : PsjsStatement
   {
      public readonly List<string> Names = new List<string>();
      public readonly List<PsjsExpression> Initials = new List<PsjsExpression>();

      public override void Run(PsjsScope scope)
      {
         for (int index = 0; index < Names.Count; index++)
            scope.Declare(Names[index], Initials[index] == null ? null : Initials[index].Evaluate(scope));
      }
   }

   public class PsjsFunctionDeclaration : PsjsStatement
   {
      public string Name;
      public PsjsFunctionLiteral Body;

      public override void Run(PsjsScope scope) { scope.Declare(Name, Body.Evaluate(scope)); }
   }

   public class PsjsIfStatement : PsjsStatement
   {
      public PsjsExpression Condition;
      public PsjsStatement WhenTrue;
      public PsjsStatement WhenFalse;

      public override void Run(PsjsScope scope)
      {
         if (PsjsValues.ToBool(Condition.Evaluate(scope))) WhenTrue.Run(scope);
         else if (WhenFalse != null) WhenFalse.Run(scope);
      }
   }

   public class PsjsWhileStatement : PsjsStatement
   {
      public PsjsExpression Condition;
      public PsjsStatement Body;

      public override void Run(PsjsScope scope)
      {
         for (int turns = 0; PsjsValues.ToBool(Condition.Evaluate(scope)); turns++) {
            PsjsValues.GuardLoop(turns, Line);
            try {
               Body.Run(scope);
            } catch (PsjsBreakSignal) {
               return;
            } catch (PsjsContinueSignal) {
               // straight on to the next turn
            }
         }
      }
   }

   public class PsjsForStatement : PsjsStatement
   {
      public PsjsStatement Setup;
      public PsjsExpression Condition;
      public PsjsExpression Step;
      public PsjsStatement Body;

      public override void Run(PsjsScope scope)
      {
         if (Setup != null) Setup.Run(scope);
         for (int turns = 0; Condition == null || PsjsValues.ToBool(Condition.Evaluate(scope)); turns++) {
            PsjsValues.GuardLoop(turns, Line);
            try {
               Body.Run(scope);
            } catch (PsjsBreakSignal) {
               return;
            } catch (PsjsContinueSignal) {
               // the step still runs, exactly as it does after an ordinary turn
            }
            if (Step != null) Step.Evaluate(scope);
         }
      }
   }

   public class PsjsBreakStatement : PsjsStatement
   {
      public override void Run(PsjsScope scope) { throw new PsjsBreakSignal(); }
   }

   public class PsjsContinueStatement : PsjsStatement
   {
      public override void Run(PsjsScope scope) { throw new PsjsContinueSignal(); }
   }

   // condition ? whenTrue : whenFalse -- only the chosen side is evaluated
   public class PsjsTernary : PsjsExpression
   {
      public PsjsExpression Condition, WhenTrue, WhenFalse;

      public override object Evaluate(PsjsScope scope)
      {
         return PsjsValues.ToBool(Condition.Evaluate(scope)) ? WhenTrue.Evaluate(scope) : WhenFalse.Evaluate(scope);
      }
   }

   // i++ / ++i / i-- / --i
   public class PsjsStep : PsjsExpression
   {
      public string Operator;
      public PsjsExpression Target;

      public override object Evaluate(PsjsScope scope)
      {
         double moved = PsjsValues.ToNumber(Target.Evaluate(scope)) + (Operator == "++" ? 1 : -1);
         PsjsAssignment.Store(Target, moved, scope, Line);
         return moved;
      }
   }

   public class PsjsArrayLiteral : PsjsExpression
   {
      public readonly List<PsjsExpression> Items = new List<PsjsExpression>();

      public override object Evaluate(PsjsScope scope)
      {
         var array = new PsjsArray();
         foreach (PsjsExpression item in Items) array.Push(item.Evaluate(scope));
         return array;
      }
   }

   public class PsjsReturnStatement : PsjsStatement
   {
      public PsjsExpression Value;

      public override void Run(PsjsScope scope)
      {
         throw new PsjsReturnSignal(Value == null ? null : Value.Evaluate(scope));
      }
   }

   // carries a return value out of however many blocks deep it was written
   public class PsjsReturnSignal : Exception
   {
      public readonly object Value;
      public PsjsReturnSignal(object value) { Value = value; }
   }

   // the same trick for leaving or restarting a loop from inside nested blocks
   public class PsjsBreakSignal : Exception { }
   public class PsjsContinueSignal : Exception { }
}
