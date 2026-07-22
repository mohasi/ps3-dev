using System;
using System.Collections.Generic;

namespace ThemeStudio
{
   // runs a scene's script on the pc so the preview can move. the console runs the real thing;
   // this covers what scripts actually use -- vectors, maths, timers and the moving setters --
   // and reports anything it does not understand rather than pretending to have run it.
   public class PsjsMachine
   {
      private readonly Dictionary<string, PsjsThing> things = new Dictionary<string, PsjsThing>();
      private readonly List<PsjsTimer> timers = new List<PsjsTimer>();
      private readonly Action<string> report;
      private double now;
      private double scriptClock;   // what the script sees as "now", which inside a timer is when it fired
      private double aspect = 16.0 / 9.0;   // 4/3 or 16/9, needed for System.resolution

      public PsjsThing Camera { get; private set; }
      public bool Faulted { get; private set; }

      // did the last Advance actually move anything? a scene at rest looks the same as it did
      // last frame, so whoever is drawing it can skip the frame entirely.
      public bool Changed { get; private set; }

      private PsjsMachine(Action<string> report) { this.report = report; }

      // builds the world from the scene, then runs the script's top level once. anything the
      // script sets up -- timers, opening values -- is in place when this returns.
      public static PsjsMachine Start(SceneProject scene, string source, Action<string> report)
      {
         return Start(scene, source, 16.0 / 9.0, report);
      }

      // aspect is what the console reports to a script as Camera.aspect -- 4/3 or 16/9 -- so it
      // comes from the view actually being drawn rather than being assumed
      public static PsjsMachine Start(SceneProject scene, string source, double aspect, Action<string> report)
      {
         var machine = new PsjsMachine(report);
         machine.aspect = aspect;
         foreach (SceneActor actor in scene.Actors) {
            // an object starts where its model asks to be, not at nothing: a script that reads
            // back its own scale before changing it has to see the same value the console sees
            Vec3 position, rotation, scale;
            SceneDefaults.StartingPlacement(scene, actor, out position, out rotation, out scale);

            var thing = new PsjsThing(actor.Id, machine);
            thing.Reset("position", new PsjsVector(position, 0));
            thing.Reset("rotation", new PsjsVector(rotation, 0));
            thing.Reset("scale", new PsjsVector(scale, 0));
            thing.Reset("color", new PsjsVector(1, 1, 1, 1));
            machine.things[actor.Id] = thing;
         }
         // lights are reached exactly as actors are -- new Light("mainlight") -- so they live in
         // the same table. their ids are fixed and cannot clash with an actor's.
         foreach (SceneLight light in scene.Lights) {
            var thing = new PsjsThing(light.Id, machine);
            thing.Reset("position", new PsjsVector(light.Position, 0));
            thing.Reset("direction", new PsjsVector(0, 0, -1, 0));
            thing.Reset("color", new PsjsVector(light.Color, 1));
            thing.Reset("attenuation", new PsjsVector(light.Attenuation, 0));
            machine.things[light.Id] = thing;
         }

         machine.Camera = new PsjsThing(SceneProject.CameraId, machine);
         machine.Camera.Reset("position", new PsjsVector(scene.CameraPosition, 0));
         machine.Camera.Reset("direction", new PsjsVector(scene.CameraDirection, 0));
         machine.Camera.Reset("up", new PsjsVector(scene.CameraUp, 0));
         machine.Camera.ResetNumber("yfov", scene.CameraFieldOfView);
         machine.Camera.ResetNumber("aspect", aspect);   // read only, as on the console
         machine.things[SceneProject.CameraId] = machine.Camera;

         try {
            PsjsBudget.Begin();
            PsjsParser.Parse(source).Run(machine.makeGlobals());
         } catch (PsjsError error) {
            machine.fault("the preview could not run the script: " + error.Message);
         } catch (Exception error) {
            machine.fault("the preview could not run the script: " + error.Message);
         }
         return machine;
      }

      public PsjsThing Find(string id)
      {
         PsjsThing thing;
         return things.TryGetValue(id, out thing) ? thing : null;
      }

      // moves the world on. timers fire first, so anything they start is already moving this frame.
      public void Advance(double seconds)
      {
         if (Faulted) return;
         PsjsBudget.Begin();   // each frame gets its own allowance
         Changed = false;
         now += seconds;
         scriptClock = now;
         fireDueTimers();
         scriptClock = now;
         settleAt(now);
      }

      private void settleAt(double when)
      {
         foreach (PsjsThing thing in things.Values)
            if (thing.SettleAt(when)) Changed = true;
      }

      private void fireDueTimers()
      {
         for (int fired = 0; fired < 200; fired++) {
            // slots the script never filled are empty: setting timer[3] first leaves 0..2 holes
            PsjsTimer due = null;
            foreach (PsjsTimer timer in timers)
               if (timer != null && timer.Live && timer.NextAt <= now &&
                   (due == null || timer.NextAt < due.NextAt)) due = timer;
            if (due == null) return;

            // the world is put where it is at the instant the timer fires, not where it was at the
            // last frame -- a repeating timer that reads a value would otherwise fall further
            // behind on every tick
            scriptClock = due.NextAt;
            settleAt(scriptClock);
            if (due.Repeating) due.NextAt += due.Interval; else due.Live = false;
            Changed = true;   // a timer can set anything, so assume the picture is now different
            try {
               due.Callback.Call(new object[0]);
            } catch (Exception error) {
               due.Live = false;
               fault("a timer in the script stopped the preview: " + error.Message);
               return;
            }
         }
      }

      internal double Now { get { return scriptClock; } }

      // the spec gives no slot count, so this is the preview's own guard, not the console's rule:
      // growing a list to reach a wild index would ask for hundreds of megabytes before failing.
      // raise it if a real theme ever needs more.
      internal const int TimerSlots = 32;

      internal void Register(int slot, PsjsTimer timer)
      {
         if (slot < 0 || slot >= TimerSlots)
            throw new PsjsError("System.timer has slots 0 to " + (TimerSlots - 1) + ", so " + slot + " is not one", 0);
         while (timers.Count <= slot) timers.Add(null);
         if (timers[slot] != null) timers[slot].Live = false;
         timer.NextAt = now + timer.Interval;
         timer.Live = true;
         timers[slot] = timer;
      }

      private void fault(string message)
      {
         if (Faulted) return;
         Faulted = true;
         report(message);
      }

      // the world the script sees

      private PsjsScope makeGlobals()
      {
         var scope = new PsjsScope(null);
         scope.Declare("Math", makeMath());
         scope.Declare("System", makeSystem());
         scope.Declare("INTERPOLATION_LINEAR", 0.0);
         scope.Declare("INTERPOLATION_BEZIER", 1.0);
         scope.Declare("Actor", new PsjsNative(findOrWarn));
         scope.Declare("Light", new PsjsNative(findOrWarn));
         scope.Declare("Camera", new PsjsNative(delegate { return Camera; }));
         scope.Declare("Date", new PsjsNative(delegate { return makeDate(); }));
         scope.Declare("IntervalTimer", new PsjsNative(arguments => makeTimer(arguments, true)));
         scope.Declare("OneShotTimer", new PsjsNative(arguments => makeTimer(arguments, false)));
         scope.Declare("Array", new PsjsNative(makeArray));
         // Sony's samples call writeln without naming System first, so both spellings work
         scope.Declare("writeln", new PsjsNative(write));
         scope.Declare("write", new PsjsNative(write));
         return scope;
      }

      // naming something that is not in the scene is the commonest mistake, and the console just
      // fails quietly, so the preview says which name it was
      private object findOrWarn(object[] arguments)
      {
         string id = arguments.Length > 0 ? PsjsValues.ToText(arguments[0]) : "";
         PsjsThing thing = Find(id);
         if (thing != null) return thing;
         report("the script uses \"" + id + "\", which is not in the scene");
         return new PsjsThing(id, this);
      }

      private object makeTimer(object[] arguments, bool repeating)
      {
         double interval = arguments.Length > 0 ? PsjsValues.ToNumber(arguments[0]) : 1.0;
         var callback = arguments.Length > 1 ? arguments[1] as IPsjsCallable : null;
         if (callback == null) {
            report("a timer was made without anything to call");
            callback = new PsjsNative(delegate { return null; });
         }
         return new PsjsTimer { Interval = Math.Max(interval, 1.0 / 60), Callback = callback, Repeating = repeating };
      }

      private object makeDate()
      {
         DateTime clock = DateTime.Now;
         var date = new PsjsBag();
         date.Add("year", (double)clock.Year);
         date.Add("month", (double)clock.Month);
         date.Add("day", (double)clock.Day);
         date.Add("hours", (double)clock.Hour);
         date.Add("minutes", (double)clock.Minute);
         date.Add("seconds", (double)clock.Second);
         return date;
      }

      private PsjsBag makeSystem()
      {
         var system = new PsjsBag();
         system.Add("timer", new PsjsTimerSlots(this));
         // the screen size the console reports, <width, height>. picked from the aspect being
         // drawn, the two the PS3 XMB uses: 1280x720 wide, 640x480 standard.
         system.Add("resolution", aspect > 1.5 ? new PsjsVector(1280, 720, 0, 0) : new PsjsVector(640, 480, 0, 0));
         system.Add("interval", 1.0 / 30);   // seconds a frame lasts, as the preview steps it
         system.AddFunction("writeln", arguments => write(arguments));
         system.AddFunction("write", arguments => write(arguments));
         system.AddFunction("printPerf", delegate { return null; });
         system.AddFunction("printHeap", delegate { return null; });
         return system;
      }

      // new Array(n) makes n empty slots; new Array(a, b, c) holds those things, which is how
      // Sony's samples build their background list
      private static object makeArray(object[] arguments)
      {
         if (arguments.Length == 1 && arguments[0] is double)
            return new PsjsArray((int)PsjsValues.ToNumber(arguments[0]));

         var array = new PsjsArray();
         foreach (object item in arguments) array.Push(item);
         return array;
      }

      private object write(object[] arguments)
      {
         var line = new System.Text.StringBuilder("script: ");
         foreach (object argument in arguments) line.Append(PsjsValues.ToText(argument));
         report(line.ToString());
         return null;
      }

      private static PsjsBag makeMath()
      {
         var maths = new PsjsBag();
         maths.Add("PI", Math.PI);
         maths.Add("E", Math.E);
         maths.AddFunction("sin", arguments => Math.Sin(firstNumber(arguments)));
         maths.AddFunction("cos", arguments => Math.Cos(firstNumber(arguments)));
         maths.AddFunction("tan", arguments => Math.Tan(firstNumber(arguments)));
         maths.AddFunction("sqrt", arguments => Math.Sqrt(Math.Abs(firstNumber(arguments))));
         maths.AddFunction("abs", arguments => Math.Abs(firstNumber(arguments)));
         maths.AddFunction("floor", arguments => Math.Floor(firstNumber(arguments)));
         maths.AddFunction("ceil", arguments => Math.Ceiling(firstNumber(arguments)));
         maths.AddFunction("round", arguments => Math.Round(firstNumber(arguments)));
         maths.AddFunction("pow", arguments => Math.Pow(firstNumber(arguments), secondNumber(arguments)));
         maths.AddFunction("atan2", arguments => Math.Atan2(firstNumber(arguments), secondNumber(arguments)));
         maths.AddFunction("min", arguments => Math.Min(firstNumber(arguments), secondNumber(arguments)));
         maths.AddFunction("max", arguments => Math.Max(firstNumber(arguments), secondNumber(arguments)));
         maths.AddFunction("random", delegate { return randomSource.NextDouble(); });
         return maths;
      }

      private static readonly Random randomSource = new Random(1);   // fixed, so a preview repeats

      private static double firstNumber(object[] arguments)
      {
         return arguments.Length > 0 ? PsjsValues.ToNumber(arguments[0]) : 0;
      }

      private static double secondNumber(object[] arguments)
      {
         return arguments.Length > 1 ? PsjsValues.ToNumber(arguments[1]) : 0;
      }
   }

   // System.timer[n] = new IntervalTimer(...). a timer only starts once it is put in a slot, which
   // is the console's own rule, and putting a new one in a slot cancels whatever was there.
   public class PsjsTimerSlots : IPsjsIndexed
   {
      private readonly PsjsMachine machine;
      private readonly Dictionary<int, PsjsTimer> slots = new Dictionary<int, PsjsTimer>();

      public PsjsTimerSlots(PsjsMachine machine) { this.machine = machine; }

      public object GetIndex(int index)
      {
         PsjsTimer timer;
         return slots.TryGetValue(index, out timer) ? timer : null;
      }

      public void SetIndex(int index, object value)
      {
         var timer = value as PsjsTimer;
         if (timer == null) return;
         slots[index] = timer;
         machine.Register(index, timer);
      }
   }

   public class PsjsTimer : IPsjsMembers
   {
      public double Interval;
      public IPsjsCallable Callback;
      public bool Repeating;
      public double NextAt;
      public bool Live;

      public object GetMember(string name) { return name == "interval" ? (object)Interval : null; }

      public void SetMember(string name, object value)
      {
         if (name == "interval") Interval = Math.Max(PsjsValues.ToNumber(value), 1.0 / 60);
      }
   }

   // one thing in the scene as the script sees it: a handful of vectors, a switch, and the moving
   // setters that ease one of those vectors to a new value over a number of seconds.
   public class PsjsThing : IPsjsMembers
   {
      // setter name -> the vector it moves
      private static readonly Dictionary<string, string> MovedByName = new Dictionary<string, string> {
         { "setPosition", "position" }, { "setRotation", "rotation" }, { "setScale", "scale" },
         { "setColor", "color" }, { "setDirection", "direction" }, { "setUp", "up" },
         { "setUVScale", "uv_scale" }, { "setUVOffset", "uv_offset" }, { "setAttenuation", "attenuation" }
      };

      // the camera alone has plain-number properties, and none of them ease -- the format gives
      // no setYfov, so they are assigned outright
      private static readonly string[] ReadOnlyNumbers = { "aspect" };

      private readonly Dictionary<string, PsjsVector> vectors = new Dictionary<string, PsjsVector>();
      private readonly Dictionary<string, double> numbers = new Dictionary<string, double>();
      private readonly Dictionary<string, PsjsEase> moves = new Dictionary<string, PsjsEase>();
      private readonly PsjsMachine machine;

      public readonly string Id;
      public bool Enable = true;

      public PsjsThing(string id, PsjsMachine machine)
      {
         Id = id;
         this.machine = machine;
      }

      public void Reset(string name, PsjsVector value) { vectors[name] = value; }
      public void ResetNumber(string name, double value) { numbers[name] = value; }

      public PsjsVector Get(string name)
      {
         PsjsVector value;
         return vectors.TryGetValue(name, out value) ? value : null;
      }

      public double GetNumber(string name, double fallback)
      {
         double value;
         return numbers.TryGetValue(name, out value) ? value : fallback;
      }

      // true when something actually moved. a move is dropped once it arrives, so a scene that has
      // finished everything it was asked to do costs nothing per frame.
      public bool SettleAt(double now)
      {
         if (moves.Count == 0) return false;

         List<string> arrived = null;
         foreach (KeyValuePair<string, PsjsEase> move in moves) {
            vectors[move.Key] = move.Value.At(now);
            if (!move.Value.HasArrived(now)) continue;
            if (arrived == null) arrived = new List<string>();
            arrived.Add(move.Key);
         }
         if (arrived != null) foreach (string name in arrived) moves.Remove(name);
         return true;
      }

      public object GetMember(string name)
      {
         if (name == "enable") return Enable;

         PsjsVector vector;
         if (vectors.TryGetValue(name, out vector)) return vector;

         double number;
         if (numbers.TryGetValue(name, out number)) return number;

         string moved;
         if (MovedByName.TryGetValue(name, out moved)) return new PsjsNative(arguments => startMove(moved, arguments));

         // animation blending. the preview cannot play a model's baked animation, so these do
         // nothing and report the resting values -- but a script that calls them still runs and
         // shows the rest of the scene, rather than stopping on an unknown method.
         if (name == "setAnimWeight" || name == "setAnimSpeed" || name == "setAnimTime")
            return new PsjsNative(delegate { return null; });
         if (name == "getAnimWeight" || name == "getAnimSpeed")
            return new PsjsNative(arguments => (object)1.0);
         if (name == "getAnimTime" || name == "getAnimIndex")
            return new PsjsNative(arguments => (object)0.0);
         return null;
      }

      public void SetMember(string name, object value)
      {
         if (name == "enable") { Enable = PsjsValues.ToBool(value); return; }

         if (numbers.ContainsKey(name)) {
            if (Array.IndexOf(ReadOnlyNumbers, name) < 0) numbers[name] = PsjsValues.ToNumber(value);
            return;
         }

         var vector = value as PsjsVector;
         if (vector == null) return;
         vectors[name] = vector.Copy();
         moves.Remove(name);   // setting a value outright stops whatever was easing it
      }

      // setSomething(value, seconds, [easing], [easing detail]) -- the shape every setter shares
      private object startMove(string name, object[] arguments)
      {
         var target = arguments.Length > 0 ? arguments[0] as PsjsVector : null;
         if (target == null) return null;

         PsjsVector from = Get(name);
         if (from == null) from = new PsjsVector();
         double seconds = arguments.Length > 1 ? PsjsValues.ToNumber(arguments[1]) : 0;

         if (seconds <= 0) {
            vectors[name] = target.Copy();
            moves.Remove(name);
            return null;
         }

         bool bezier = arguments.Length > 2 && PsjsValues.ToNumber(arguments[2]) != 0;
         var control = arguments.Length > 3 ? arguments[3] as PsjsVector : null;
         moves[name] = new PsjsEase {
            From = from.Copy(), To = target.Copy(), Start = machine.Now, Duration = seconds,
            Bezier = bezier, Control = control
         };
         return null;
      }
   }

   // one value on its way to another
   public class PsjsEase
   {
      public PsjsVector From, To, Control;
      public double Start, Duration;
      public bool Bezier;

      public bool HasArrived(double now) { return now >= Start + Duration; }

      public PsjsVector At(double now)
      {
         double fraction = Duration <= 0 ? 1 : (now - Start) / Duration;
         fraction = fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);
         return PsjsVector.Mix(From, To, Bezier ? shape(fraction) : fraction);
      }

      // the console eases along a cubic bezier given as <x1, y1, x2, y2>. the curve gives y for a
      // given t, but what is wanted is y for a given x, so x is solved for first by halving.
      private double shape(double fraction)
      {
         if (Control == null) return fraction * fraction * (3 - 2 * fraction);   // a plain smooth start and stop

         double low = 0, high = 1;
         for (int step = 0; step < 24; step++) {
            double middle = (low + high) / 2;
            if (curve(middle, Control.X, Control.Z) < fraction) low = middle; else high = middle;
         }
         return curve((low + high) / 2, Control.Y, Control.W);
      }

      private static double curve(double t, double first, double second)
      {
         double inverse = 1 - t;
         return 3 * inverse * inverse * t * first + 3 * inverse * t * t * second + t * t * t;
      }
   }
}
