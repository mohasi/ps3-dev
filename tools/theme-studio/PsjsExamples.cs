using System;
using System.Collections.Generic;

namespace ThemeStudio
{
   // one thing a script can do, ready to paste in. each example names an object that is actually
   // in the scene, so it runs as soon as it is inserted rather than needing to be edited first.
   public class PsjsExample
   {
      public string Title;
      public string Summary;
      public Func<IList<SceneActor>, string> Build;
   }

   public static class PsjsExamples
   {
      public static readonly PsjsExample[] All = {
         new PsjsExample { Title = "Starting point", Summary = "The whole shape of a script, explained",
                           Build = PsjsSnippets.MakeStarter },
         new PsjsExample { Title = "Turn for ever", Summary = "A quarter turn every second, repeating",
                           Build = actors => turn(nameIn(actors)) },
         new PsjsExample { Title = "Fade in once", Summary = "Rise from invisible when the theme starts",
                           Build = actors => fade(nameIn(actors)) },
         new PsjsExample { Title = "Grow and shrink", Summary = "Breathe in and out, repeating",
                           Build = actors => pulse(nameIn(actors)) },
         new PsjsExample { Title = "Drift side to side", Summary = "Move between two places, repeating",
                           Build = actors => drift(nameIn(actors)) },
         new PsjsExample { Title = "Change with the clock", Summary = "Look different by time of day",
                           Build = actors => clock(nameIn(actors)) },
         new PsjsExample { Title = "Hide, then show later", Summary = "Switch an object off and back on",
                           Build = actors => hide(nameIn(actors)) },
         new PsjsExample { Title = "Move the camera", Summary = "Pull the whole view back over time",
                           Build = actors => camera() },
         new PsjsExample { Title = "Change the lighting", Summary = "Drift from warm to cool and back",
                           Build = actors => lightColour() },
         new PsjsExample { Title = "Swing the light around", Summary = "Move the main light, so shading shifts",
                           Build = actors => lightMove() }
      };

      private static string nameIn(IList<SceneActor> actors)
      {
         return actors.Count > 0 ? actors[0].Id : "myObject";
      }

      private static string turn(string id)
      {
         return @"// turn a quarter circle every second, for ever
var spinner = new Actor(""" + id + @""");
var turned = 0.0;
function spin()
{
   turned += Math.PI / 2.0;
   spinner.setRotation(<0.0, turned, 0.0>, 1.0);
}
System.timer[0] = new IntervalTimer(1.0, spin);
spin();
";
      }

      private static string fade(string id)
      {
         return @"// rise from invisible, gently, once when the theme starts
var fading = new Actor(""" + id + @""");
fading.color = <1.0, 1.0, 1.0, 0.0>;
fading.setColor(<1.0, 1.0, 1.0, 1.0>, 2.0, INTERPOLATION_BEZIER, <0.1, 0.0, 0.1, 1.0>);
";
      }

      private static string pulse(string id)
      {
         return @"// breathe in and out, for ever
var pulsing = new Actor(""" + id + @""");
var big = 0;
function breathe()
{
   big = 1 - big;
   if (big == 1) pulsing.setScale(<1.3, 1.3, 1.3>, 1.5, INTERPOLATION_BEZIER, <0.4, 0.0, 0.6, 1.0>);
   else pulsing.setScale(<1.0, 1.0, 1.0>, 1.5, INTERPOLATION_BEZIER, <0.4, 0.0, 0.6, 1.0>);
}
System.timer[1] = new IntervalTimer(1.5, breathe);
";
      }

      private static string drift(string id)
      {
         return @"// drift between two places, for ever
var drifting = new Actor(""" + id + @""");
var side = 0;
function slide()
{
   side = 1 - side;
   if (side == 1) drifting.setPosition(<0.5, 0.0, 0.0>, 3.0);
   else drifting.setPosition(<-0.5, 0.0, 0.0>, 3.0);
}
System.timer[2] = new IntervalTimer(3.0, slide);
slide();
";
      }

      private static string clock(string id)
      {
         return @"// look different depending on the time of day
var timed = new Actor(""" + id + @""");
var now = new Date();
if (now.hours < 12) timed.color = <1.0, 0.95, 0.8, 1.0>;        // morning, warm
else if (now.hours < 18) timed.color = <1.0, 1.0, 1.0, 1.0>;    // afternoon, plain
else timed.color = <0.7, 0.75, 1.0, 1.0>;                       // evening, cool
";
      }

      private static string hide(string id)
      {
         return @"// a script cannot create an object, so include it and switch it off until wanted
var later = new Actor(""" + id + @""");
later.enable = false;
function reveal()
{
   later.enable = true;
}
System.timer[3] = new OneShotTimer(3.0, reveal);
";
      }

      private static string camera()
      {
         return @"// the camera is an object like any other, so the whole view can move.
// every scene has exactly one and it is always called """ + SceneProject.CameraId + @""".
var view = new Camera(""" + SceneProject.CameraId + @""");

// pull back over 4 seconds, slowing as it arrives
view.setPosition(<0.0, 0.0, 8.0>, 4.0, INTERPOLATION_BEZIER, <0.2, 0.0, 0.2, 1.0>);

// other things the camera can do:
//   view.setDirection(<0.0, 0.0, -1.0>, 2.0);   turn to face somewhere else
//   view.setUp(<1.0, 0.0, 0.0>, 2.0);           roll the whole picture over
//   view.yfov = 1.2;                            widen the view (radians, no fade)
//   view.aspect                                 4/3 or 16/9, read only
";
      }

      private static string lightColour()
      {
         return @"// every scene has two lights, always called """ + SceneProject.MainLightId + @""" and """ +
                SceneProject.FillLightId + @""".
// """ + SceneProject.MainLightId + @""" shines from a point; """ + SceneProject.FillLightId + @""" is an even glow from everywhere.
var main = new Light(""" + SceneProject.MainLightId + @""");
var fill = new Light(""" + SceneProject.FillLightId + @""");

// warm the scene up over 5 seconds, then cool it down, for ever.
// a light's colour is <red, green, blue>, each 0 to 1.
var warm = 0;
function shift()
{
   warm = 1 - warm;
   if (warm == 1) main.setColor(<1.0, 0.7, 0.4>, 5.0);
   else main.setColor(<0.5, 0.7, 1.0>, 5.0);
}
System.timer[0] = new IntervalTimer(5.0, shift);
shift();

// turn the fill light right down, so unlit sides fall into shadow
fill.setColor(<0.1, 0.1, 0.1>, 2.0);
";
      }

      private static string lightMove()
      {
         return @"// swing the main light around the scene, so the shading changes as it goes
var main = new Light(""" + SceneProject.MainLightId + @""");
var angle = 0.0;

function swing()
{
   angle += Math.PI / 8.0;
   main.setPosition(<Math.cos(angle) * 3.0, 1.0, Math.sin(angle) * 3.0>, 1.0);
}
System.timer[0] = new IntervalTimer(1.0, swing);
swing();

// how quickly the light fades with distance: <flat, with distance, with distance squared>.
// bigger numbers mean it falls off sooner.
main.setAttenuation(<0.0, 1.0, 4.0>, 3.0);
";
      }
   }
}
