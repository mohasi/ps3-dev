using System;
using System.Diagnostics;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Media3D;
using System.Windows.Threading;

namespace ThemeStudio
{
   // plays a scene: runs its script on a clock and moves the built shapes to match. the console
   // does the same thing with its own engine, so this is a likeness, not the article -- but the
   // movement a script describes is arithmetic, and arithmetic carries over exactly.
   public class ScenePlayer
   {
      private const int FramesPerSecond = 30;

      private readonly SceneView view;
      private readonly PsjsMachine machine;
      private readonly DispatcherTimer clock;
      private readonly Stopwatch elapsed = new Stopwatch();
      private readonly double aspect;
      private double lastSeconds;

      public Viewport3D Viewport { get { return view.Viewport; } }

      private ScenePlayer(SceneView view, PsjsMachine machine, double aspect)
      {
         this.view = view;
         this.machine = machine;
         this.aspect = aspect;
         clock = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1.0 / FramesPerSecond) };
         clock.Tick += onTick;
      }

      public static ScenePlayer Start(ThemeProject project, string script, double width, double height,
                                      Action<string> report)
      {
         SceneView view = ScenePreview.Build(project.Scene, project.ContentDir, width, height, report);
         var player = new ScenePlayer(view, PsjsMachine.Start(project.Scene, script, width / height, report), width / height);
         player.showFrame();
         player.elapsed.Start();
         player.clock.Start();
         return player;
      }

      public void Stop()
      {
         clock.Stop();
         elapsed.Stop();
      }

      // paused when the window is not the one being used: a scene nobody is watching still costs
      // a whole core to draw. the stopwatch stops with it, so the scene carries on where it was
      // rather than jumping forward by however long you were away.
      public void SetRunning(bool running)
      {
         if (running == clock.IsEnabled) return;
         if (running) { elapsed.Start(); clock.Start(); } else { clock.Stop(); elapsed.Stop(); }
      }

      private void onTick(object sender, EventArgs e)
      {
         double seconds = elapsed.Elapsed.TotalSeconds;
         machine.Advance(seconds - lastSeconds);
         lastSeconds = seconds;

         // a scene between moves is the same picture as last frame, so drawing it again is work
         // for nothing -- and most themes sit still most of the time
         if (machine.Changed) {
            showFrame();
            if (Ticked != null) Ticked();
         }
         if (machine.Faulted) Stop();   // a broken script freezes rather than repeating its error
      }

      // called after every frame, so a readout of what the camera is doing can keep up with it
      public Action Ticked;

      private void showFrame()
      {
         foreach (var pair in view.ShapeByActorId) {
            PsjsThing thing = machine.Find(pair.Key);
            if (thing == null) continue;
            showShape(pair.Value, thing);
         }
         showLights();
         showCamera();
      }

      // a script can move a light, recolour it and change how fast it falls off. wpf has no
      // attenuation to match the console's, so that one moves the value but not the picture.
      private void showLights()
      {
         foreach (var pair in view.LightById) {
            PsjsThing thing = machine.Find(pair.Key);
            if (thing == null) continue;

            PsjsVector colour = thing.Get("color");
            if (colour != null) pair.Value.Color = toColour(colour);

            var point = pair.Value as PointLight;
            PsjsVector position = thing.Get("position");
            if (point != null && position != null) point.Position = new Point3D(position.X, position.Y, position.Z);

            GeometryModel3D marker;
            if (!view.MarkerByLightId.TryGetValue(pair.Key, out marker)) continue;
            if (position != null)
               marker.Transform = new TranslateTransform3D(position.X, position.Y, position.Z);
            var glow = marker.Material as EmissiveMaterial;
            if (glow != null && colour != null) glow.Brush = new SolidColorBrush(toColour(colour));
         }
      }

      private static Color toColour(PsjsVector value) { return ScenePreview.ToColor(value.ToVec3()); }

      private void showShape(GeometryModel3D shape, PsjsThing thing)
      {
         // the matrix is replaced in place; building a new transform for every object every frame
         // was the editor's own biggest per-frame cost
         var placement = shape.Transform as MatrixTransform3D;
         if (placement != null)
            placement.Matrix = ScenePreview.MakeMatrix(toVec3(thing.Get("position")), toVec3(thing.Get("rotation")),
                                                       toVec3(thing.Get("scale")));
         // an object switched off keeps its place in the scene but is not drawn, exactly as
         // "enable = false" does on the console
         bool inScene = view.Root.Children.Contains(shape);
         if (thing.Enable && !inScene) view.Root.Children.Add(shape);
         else if (!thing.Enable && inScene) view.Root.Children.Remove(shape);

         tint(shape.Material, thing.Get("color"));
         tint(shape.BackMaterial, thing.Get("color"));
      }

      // the script's colour multiplies the surface, alpha included -- how a fade in or out is done
      private static void tint(Material material, PsjsVector colour)
      {
         var diffuse = material as DiffuseMaterial;
         if (diffuse == null || colour == null) return;
         diffuse.Color = Color.FromArgb(ScenePreview.ToByte(colour.W), ScenePreview.ToByte(colour.X),
                                        ScenePreview.ToByte(colour.Y), ScenePreview.ToByte(colour.Z));
      }

      private void showCamera()
      {
         PsjsVector position = machine.Camera.Get("position");
         PsjsVector direction = machine.Camera.Get("direction");
         PsjsVector up = machine.Camera.Get("up");
         if (position != null) view.Camera.Position = new Point3D(position.X, position.Y, position.Z);
         if (direction != null && (direction.X != 0 || direction.Y != 0 || direction.Z != 0))
            view.Camera.LookDirection = new Vector3D(direction.X, direction.Y, direction.Z);
         if (up != null && (up.X != 0 || up.Y != 0 || up.Z != 0))
            view.Camera.UpDirection = new Vector3D(up.X, up.Y, up.Z);

         double fieldOfView = machine.Camera.GetNumber("yfov", 0);
         if (fieldOfView > 0) view.Camera.FieldOfView = ScenePreview.ToHorizontalDegrees(fieldOfView, aspect);
      }

      // the camera cannot appear in its own view, so what it is doing is reported in words instead
      public string GetCameraReadout()
      {
         PsjsVector position = machine.Camera.Get("position");
         if (position == null) return "";
         double degrees = machine.Camera.GetNumber("yfov", 0) * 180.0 / Math.PI;
         return "camera at " + round(position.X) + ", " + round(position.Y) + ", " + round(position.Z) +
                "   -   " + round(degrees) + " degrees wide";
      }

      private static string round(double value)
      {
         return value.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture);
      }

      public void ShowMarkers(bool showing) { view.ShowMarkers(showing); }

      private static Vec3 toVec3(PsjsVector value) { return value == null ? new Vec3() : value.ToVec3(); }
   }
}
