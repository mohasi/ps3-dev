using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace ThemeStudio
{
   // PSJS is Sony's own javascript dialect, not plain javascript: it has vector literals
   // written <x, y, z>, uses -> to reach one component, and drops for...in, with, and `this`.
   // pasting ordinary javascript in usually fails, so the editor hands out working examples and
   // suggests only names that actually exist.
   public static class PsjsSnippets
   {
      // everything the language offers, from Sony's RAF specification. small enough to list in
      // full, which is what makes suggesting names worthwhile. split by where a name may appear,
      // because that is exactly what the suggestion popup has to know: what follows a dot can
      // only be one of the members.
      public static readonly string[] GlobalNames = {
         "Actor", "Camera", "Light", "Date", "System", "Math", "IntervalTimer", "OneShotTimer",
         "INTERPOLATION_LINEAR", "INTERPOLATION_BEZIER"
      };

      // what each kind of object offers. kept apart because a camera has no scale and a light has
      // no rotation: offering them anyway would say the language allows something it does not.
      public static readonly string[] ActorMembers = {
         "setPosition", "setRotation", "setDirection", "setUp", "setScale", "setColor",
         "setUVScale", "setUVOffset", "setAnimWeight", "setAnimSpeed", "setAnimTime",
         "getAnimWeight", "getAnimSpeed", "getAnimTime", "getAnimIndex",
         "position", "rotation", "direction", "up", "scale", "color", "enable",
         "uv_scale", "uv_offset", "timer"
      };

      public static readonly string[] CameraMembers = {
         "setPosition", "setDirection", "setUp", "position", "direction", "up", "yfov", "ymag", "aspect"
      };

      public static readonly string[] LightMembers = {
         "setPosition", "setDirection", "setColor", "setAttenuation",
         "position", "direction", "color", "attenuation"
      };

      public static readonly string[] SystemMembers = {
         "timer", "interval", "resolution", "printPerf", "printHeap", "writeln", "write"
      };

      public static readonly string[] DateMembers = { "hours", "minutes", "seconds", "year", "month", "day" };

      public static readonly string[] MathNames = { "PI", "sin", "cos", "random", "floor", "abs" };

      // the parts of a point in space, reached with ->
      public static readonly string[] PartNames = { "x", "y", "z", "w" };

      // every member of every kind, for the suggestion list that is not about one object
      public static readonly string[] MemberNames =
         combine(ActorMembers, CameraMembers, LightMembers, SystemMembers, DateMembers);

      // what a script means by "new Actor(...)" and so on. an unknown name has no members to
      // offer, and offering the wrong ones is worse than offering none.
      public static string[] GetMembersOf(string typeName)
      {
         switch (typeName) {
            case "Actor":  return ActorMembers;
            case "Camera": return CameraMembers;
            case "Light":  return LightMembers;
            case "Date":   return DateMembers;
            default:       return null;
         }
      }

      // what to show beside a name in the suggestion list: how it is called, or what it holds.
      // "seconds" left out means the value changes at once rather than moving to it.
      private const string PointOverTime = "(<x, y, z>, seconds, [easing], [shape])";
      private const string BakedAnimation = "the model's own baked animation -- not shown in the preview";

      private static readonly Dictionary<string, string> HintByName = new Dictionary<string, string> {
         { "setPosition",   PointOverTime },
         { "setRotation",   "(<x, y, z> in radians, seconds, [easing], [shape])" },
         { "setDirection",  PointOverTime },
         { "setUp",         PointOverTime },
         { "setScale",      PointOverTime },
         { "setColor",      "(<red, green, blue, howSolid>, seconds, [easing], [shape])" },
         { "setUVScale",    "(<u, v>, seconds, [easing], [shape])   stretches the texture" },
         { "setUVOffset",   "(<u, v>, seconds, [easing], [shape])   slides the texture" },
         { "setAttenuation","(<constant, linear, square>, seconds)   how the light fades with distance" },
         { "setAnimWeight", BakedAnimation },
         { "setAnimSpeed",  BakedAnimation },
         { "setAnimTime",   BakedAnimation },
         { "getAnimWeight", BakedAnimation },
         { "getAnimSpeed",  BakedAnimation },
         { "getAnimTime",   BakedAnimation },
         { "getAnimIndex",  BakedAnimation },
         { "position",      "<x, y, z>" },
         { "rotation",      "<x, y, z> in radians" },
         { "direction",     "<x, y, z>   which way it faces" },
         { "up",            "<x, y, z>   which way is up" },
         { "scale",         "<x, y, z>" },
         { "color",         "<red, green, blue, howSolid>, each 0 to 1" },
         { "attenuation",   "<constant, linear, square>" },
         { "enable",        "true or false; false hides it" },
         { "uv_scale",      "<u, v>" },
         { "uv_offset",     "<u, v>" },
         { "aspect",        "16/9 or 4/3   read only" },
         { "yfov",          "how tall the view is, in radians" },
         { "ymag",          "how tall the view is when the camera is flat-on" },
         { "timer",         "timer[0] = new IntervalTimer(seconds, whatToRun)" },
         { "interval",      "seconds the last frame took" },
         { "resolution",    "<width, height> of the screen" },
         { "printPerf",     "()   nothing to show in the preview" },
         { "printHeap",     "()   nothing to show in the preview" },
         { "writeln",       "(text)" },
         { "write",         "(text)" },
         { "hours",         "0 to 23, from new Date()" },
         { "minutes",       "0 to 59, from new Date()" },
         { "seconds",       "0 to 59, from new Date()" },
         { "year",          "from new Date()" },
         { "month",         "from new Date()" },
         { "day",           "from new Date()" },
         { "Actor",         "new Actor(\"name\")   one of your models" },
         { "Camera",        "new Camera(\"camera\")   the view itself" },
         { "Light",         "new Light(\"mainlight\")   or \"filllight\"" },
         { "Date",          "new Date()   the console's clock" },
         { "IntervalTimer", "new IntervalTimer(seconds, whatToRun)   again and again" },
         { "OneShotTimer",  "new OneShotTimer(seconds, whatToRun)   once" },
         { "INTERPOLATION_LINEAR", "a steady move" },
         { "INTERPOLATION_BEZIER", "starts and stops gently; takes a <x1, y1, x2, y2> shape" }
      };

      public static string GetHint(string name)
      {
         string hint;
         return HintByName.TryGetValue(name, out hint) ? hint : "";
      }

      private static string[] combine(params string[][] lists)
      {
         var all = new List<string>();
         foreach (string[] list in lists)
            foreach (string name in list)
               if (!all.Contains(name)) all.Add(name);
         return all.ToArray();
      }

      // A model carries two different things, and they are not treated the same.
      //
      //   1. the shape's own coordinates -- the vertex numbers, and the skeleton of a skinned
      //      model. these cannot be written as properties of an object at all, so they stay
      //      where they are and are never touched.
      //
      //   2. the placement the file asks for -- a translate, a turn and a size on the node
      //      holding the shape. these ARE the object's position, rotation and scale, so they are
      //      written out here as ordinary script lines. the console would apply them anyway; the
      //      point is that they are visible and can be changed.
      //
      // A model that asks for nothing still has to go somewhere, so the editor chooses -- and says
      // so, in the same visible way. `slot` keeps two such models from landing on top of each other.
      public static string MakePlacement(string id, DaeInfo model, SceneProject scene, int slot)
      {
         var text = new StringBuilder();
         text.Append("var " + id + " = new Actor(\"" + id + "\");\r\n");

         if (!model.HasDefaults || !asksToBePlaced(model)) {
            double growth = ScenePlacement.GetGrowth(scene, model);
            Vec3 spot = ScenePlacement.GetFreeSpot(scene, model, growth, slot);

            text.Append("// \"" + id + "\" asks for no placement of its own, so these two lines are the\r\n");
            text.Append("// editor's choice rather than the model's. It is drawn " +
                        number(model.DrawnLargestSide) + " across, so it is\r\n");
            text.Append("// sized to about a third of the screen and put in the bottom right corner,\r\n");
            text.Append("// clear of the XMB's own menu. Change them freely.\r\n");
            text.Append(id + ".scale    = " + vector(new Vec3(growth, growth, growth)) + ";\r\n");
            text.Append(id + ".position = " + vector(spot) + ";\r\n");
            return text.ToString();
         }

         text.Append("// These three are read straight out of the model, which asks to be put here.\r\n");
         text.Append("// They are what the console would use anyway; they are written out so you can\r\n");
         text.Append("// see them and change them.\r\n");
         text.Append(id + ".position = " + vector(model.DefaultPosition) + ";\r\n");
         text.Append(id + ".rotation = " + vector(model.DefaultRotation) + ";   // radians\r\n");
         text.Append(id + ".scale    = " + vector(model.DefaultScale) + ";\r\n");
         if (!model.DefaultsAreExact)
            text.Append("// NOTE: this model turns about an odd axis, so the rotation above is the\r\n" +
                        "//       nearest fit rather than exactly what the file says.\r\n");
         return text.ToString();
      }

      private static bool asksToBePlaced(DaeInfo model)
      {
         return !isAt(model.DefaultPosition, 0) || !isAt(model.DefaultRotation, 0) || !isAt(model.DefaultScale, 1);
      }

      private static bool isAt(Vec3 value, double every)
      {
         return Math.Abs(value.X - every) < 0.0001 && Math.Abs(value.Y - every) < 0.0001 &&
                Math.Abs(value.Z - every) < 0.0001;
      }

      public static string vector(Vec3 value)
      {
         return "<" + number(value.X) + ", " + number(value.Y) + ", " + number(value.Z) + ">";
      }

      public static string number(double value)
      {
         return value.ToString("0.####", CultureInfo.InvariantCulture);
      }

      // the api, the fixtures every scene has, and whatever the user has put in this one
      public static IEnumerable<string> GetApiNames(IEnumerable<SceneActor> actors)
      {
         foreach (SceneActor actor in actors) yield return "\"" + actor.Id + "\"";
         yield return "\"" + SceneProject.CameraId + "\"";
         yield return "\"" + SceneProject.MainLightId + "\"";
         yield return "\"" + SceneProject.FillLightId + "\"";
         foreach (string name in GlobalNames) yield return name;
         foreach (string name in MemberNames) yield return name;
      }

      // a starting script that already refers to the objects in this scene. mostly commented
      // out, so it explains the language without doing anything unexpected.
      public static string MakeStarter(IList<SceneActor> actors)
      {
         string firstId = actors.Count > 0 ? actors[0].Id : "myObject";
         var text = new StringBuilder();

         text.Append("// This is PSJS, Sony's own version of JavaScript. Three things to know:\r\n");
         text.Append("//\r\n");
         text.Append("// 1. Three numbers in angle brackets are a point in space: <x, y, z>\r\n");
         text.Append("//    x is left and right, y is up and down, z is towards you and away.\r\n");
         text.Append("//    Colours are four numbers instead: <red, green, blue, how solid>, each 0 to 1.\r\n");
         text.Append("//    Use -> to reach one of them on its own, like  thing.rotation->y\r\n");
         text.Append("//\r\n");
         text.Append("// 2. Anything that moves is set the same way:\r\n");
         text.Append("//       thing.setPosition(where, howLongInSeconds, easing, easingShape)\r\n");
         text.Append("//    where            the value to end up at\r\n");
         text.Append("//    howLongInSeconds how long the move takes; 0 means jump there at once\r\n");
         text.Append("//    easing           leave it out for a steady move, or INTERPOLATION_BEZIER\r\n");
         text.Append("//                     to start and stop gently\r\n");
         text.Append("//    easingShape      only with BEZIER: <x1, y1, x2, y2> controls how gently.\r\n");
         text.Append("//                     <0.4, 0.0, 0.6, 1.0> is a good all-rounder.\r\n");
         text.Append("//\r\n");
         text.Append("// 3. A script can change the things listed on the left, but it can never add\r\n");
         text.Append("//    or remove them. To make something appear part-way through, put it in the\r\n");
         text.Append("//    scene now and hide it with  thing.enable = false;  then switch it back on.\r\n");
         text.Append("\r\n");

         if (actors.Count == 0) {
            // the compiler rejects a file that is nothing but comments, so a script with no scene
            // to talk about still has to say something
            text.Append("// Add a model on the left first, then its name can be used here.\r\n");
            text.Append("var ready = 0;\r\n");
            return text.ToString();
         }

         text.Append("// The things in this scene:\r\n");
         foreach (SceneActor actor in actors)
            text.Append("//   \"" + actor.Id + "\"\r\n");
         text.Append("// and the three every scene has, which can be moved and changed like any other:\r\n");
         text.Append("//   \"" + SceneProject.CameraId + "\"      the view itself\r\n");
         text.Append("//   \"" + SceneProject.MainLightId + "\"   the light that casts the shading\r\n");
         text.Append("//   \"" + SceneProject.FillLightId + "\"   an even glow, filling in what the main light misses\r\n");
         text.Append("\r\n");

         text.Append("// take hold of one of them, so the rest of the script can talk about it\r\n");
         text.Append("var thing = new Actor(\"" + firstId + "\");\r\n");
         text.Append("\r\n");
         text.Append("// turn a quarter circle every second, for ever.\r\n");
         text.Append("// Math.PI / 2.0 is a quarter of a full turn, measured the way scripts measure\r\n");
         text.Append("// angles: a full circle is 2 x Math.PI rather than 360.\r\n");
         text.Append("var turn = 0.0;\r\n");
         text.Append("function spin()\r\n");
         text.Append("{\r\n");
         text.Append("   turn += Math.PI / 2.0;\r\n");
         text.Append("   thing.setRotation(<0.0, turn, 0.0>, 1.0);   // turn around the up axis, over 1 second\r\n");
         text.Append("}\r\n");
         text.Append("// an IntervalTimer runs something again and again; the 1.0 is the gap in seconds.\r\n");
         text.Append("// timers live in numbered slots, System.timer[0], [1] and so on, one timer each.\r\n");
         text.Append("System.timer[0] = new IntervalTimer(1.0, spin);\r\n");
         text.Append("spin();   // the timer waits a second before its first go, so start it off by hand\r\n");
         text.Append("\r\n");
         text.Append("// fade it in once, gently, when the theme starts.\r\n");
         text.Append("// the last number of a colour is how solid it is: 0.0 invisible, 1.0 fully there.\r\n");
         text.Append("thing.color = <1.0, 1.0, 1.0, 0.0>;   // start invisible, with no fade\r\n");
         text.Append("thing.setColor(<1.0, 1.0, 1.0, 1.0>, 2.0, INTERPOLATION_BEZIER, <0.1, 0.0, 0.1, 1.0>);\r\n");
         text.Append("\r\n");
         text.Append("// Other things you can do, once you want them:\r\n");
         text.Append("//   thing.setPosition(<1.0, 0.0, 0.0>, 2.0);   slide 1 to the right, over 2 seconds\r\n");
         text.Append("//   thing.setScale(<2.0, 2.0, 2.0>, 1.0);      grow to twice the size, over 1 second\r\n");
         text.Append("//   thing.enable = false;                      hide it; true brings it back\r\n");
         text.Append("//   var now = new Date();                      the console's clock:\r\n");
         text.Append("//                                              now.hours, now.minutes, now.seconds\r\n");
         text.Append("//   var view = new Camera(\"" + SceneProject.CameraId + "\");           move the whole view:\r\n");
         text.Append("//   view.setPosition(<0, 0, 6>, 3.0);          pull back over 3 seconds\r\n");
         text.Append("//   var main = new Light(\"" + SceneProject.MainLightId + "\");      change the lighting:\r\n");
         text.Append("//   main.setColor(<1.0, 0.7, 0.4>, 4.0);       warm it up over 4 seconds\r\n");
         text.Append("//   System.timer[1] = new OneShotTimer(5.0, somethingLater);   run once, 5s from now\r\n");
         return text.ToString();
      }
   }
}
