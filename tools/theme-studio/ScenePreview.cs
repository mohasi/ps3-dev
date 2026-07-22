using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Imaging;
using System.Windows.Media;
using System.Windows.Media.Media3D;

namespace ThemeStudio
{
   // the built scene, kept in pieces so a player can move them frame by frame instead of
   // rebuilding -- decoding every texture thirty times a second would not keep up
   public class SceneView
   {
      public Viewport3D Viewport;
      public PerspectiveCamera Camera;
      public Model3DGroup Root;
      public readonly Dictionary<string, GeometryModel3D> ShapeByActorId = new Dictionary<string, GeometryModel3D>();

      // the lights themselves, so a script changing one is seen; and a visible dot for each,
      // which belongs to the editor rather than the theme and can be switched off
      public readonly Dictionary<string, Light> LightById = new Dictionary<string, Light>();
      public readonly Dictionary<string, GeometryModel3D> MarkerByLightId = new Dictionary<string, GeometryModel3D>();

      public void ShowMarkers(bool showing)
      {
         foreach (GeometryModel3D marker in MarkerByLightId.Values) {
            bool inScene = Root.Children.Contains(marker);
            if (showing && !inScene) Root.Children.Add(marker);
            else if (!showing && inScene) Root.Children.Remove(marker);
         }
      }
   }

   // builds the 3D scene the way the console's camera would see it. placement, size and colour
   // are faithful because they are plain geometry; shading is only an approximation of the
   // console's shaders.
   public static class ScenePreview
   {
      public static SceneView Build(SceneProject scene, string projectDir, double width, double height)
      {
         SceneDefaults.Fill(scene, stored => resolve(projectDir, stored));

         var view = new SceneView { Root = new Model3DGroup() };
         view.Camera = makeCamera(scene, width / height);
         view.Viewport = new Viewport3D { Width = width, Height = height, Camera = view.Camera };

         addLights(view, scene);
         addActors(view, scene, projectDir);
         view.Viewport.Children.Add(new ModelVisual3D { Content = view.Root });
         return view;
      }

      // the console describes its camera by a direction vector and a vertical field of view
      private static PerspectiveCamera makeCamera(SceneProject scene, double aspect)
      {
         Vector3D direction = toVector(scene.CameraDirection);
         if (direction.Length == 0) direction = new Vector3D(0, 0, -1);

         return new PerspectiveCamera {
            Position = toPoint(scene.CameraPosition),
            LookDirection = direction,
            UpDirection = toVector(scene.CameraUp),
            FieldOfView = ToHorizontalDegrees(scene.CameraFieldOfView, aspect),
            NearPlaneDistance = Math.Max(scene.CameraNear, 0.001),
            FarPlaneDistance = scene.CameraFar
         };
      }

      // wpf wants a horizontal angle in degrees; the console stores vertical radians
      public static double ToHorizontalDegrees(double verticalRadians, double aspect)
      {
         double horizontal = 2 * Math.Atan(Math.Tan(verticalRadians / 2) * aspect) * 180.0 / Math.PI;
         return horizontal > 0 ? horizontal : verticalRadians * 180.0 / Math.PI;
      }

      private static void addLights(SceneView view, SceneProject scene)
      {
         foreach (SceneLight light in scene.Lights) {
            Color colour = ToColor(light.Color);
            Light made = light.Type == "ambient" ? (Light)new AmbientLight(colour)
                                                 : new PointLight(colour, toPoint(light.Position));
            view.Root.Children.Add(made);
            view.LightById[light.Id] = made;

            // an ambient light shines from everywhere, so it has nowhere to draw a dot
            if (light.Type != "ambient") view.MarkerByLightId[light.Id] = makeMarker(scene, light);
         }
         // without a light every actor renders black, which would read as a broken model
         if (scene.Lights.Count == 0) view.Root.Children.Add(new AmbientLight(Colors.White));
      }

      // a small ball at the light's position, glowing its own colour. emissive so it stays visible
      // whatever the lighting is doing -- including when the light it stands for is off.
      private static GeometryModel3D makeMarker(SceneProject scene, SceneLight light)
      {
         // sized against what the camera can see, so it reads the same in a big scene and a small one
         var marker = new GeometryModel3D(makeBall(scene.GetVisibleHeight() * 0.02),
                                          new EmissiveMaterial(new SolidColorBrush(ToColor(light.Color))));
         marker.Transform = new TranslateTransform3D(light.Position.X, light.Position.Y, light.Position.Z);
         return marker;
      }

      private static MeshGeometry3D makeBall(double radius)
      {
         const int Bands = 8;
         var mesh = new MeshGeometry3D();
         for (int ring = 0; ring <= Bands; ring++) {
            double down = Math.PI * ring / Bands;
            for (int step = 0; step <= Bands; step++) {
               double around = 2 * Math.PI * step / Bands;
               mesh.Positions.Add(new Point3D(radius * Math.Sin(down) * Math.Cos(around),
                                              radius * Math.Cos(down),
                                              radius * Math.Sin(down) * Math.Sin(around)));
            }
         }
         for (int ring = 0; ring < Bands; ring++)
            for (int step = 0; step < Bands; step++) {
               int corner = ring * (Bands + 1) + step;
               addTriangle(mesh, corner, corner + Bands + 1, corner + 1);
               addTriangle(mesh, corner + 1, corner + Bands + 1, corner + Bands + 2);
            }
         mesh.Freeze();
         return mesh;
      }

      private static void addTriangle(MeshGeometry3D mesh, int first, int second, int third)
      {
         mesh.TriangleIndices.Add(first);
         mesh.TriangleIndices.Add(second);
         mesh.TriangleIndices.Add(third);
      }

      private static void addActors(SceneView view, SceneProject scene, string projectDir)
      {
         var meshCache = new Dictionary<string, MeshGeometry3D>();
         var brushCache = new Dictionary<string, Brush>();

         foreach (SceneActor actor in scene.Actors) {
            SceneModel model = scene.FindModel(actor.ModelId);
            if (model == null) continue;

            MeshGeometry3D mesh;
            if (!meshCache.TryGetValue(model.Id, out mesh)) {
               mesh = DaeFile.LoadMesh(resolve(projectDir, model.DaePath));
               meshCache[model.Id] = mesh;
            }
            if (mesh == null) continue;

            Vec3 position, rotation, scale;
            SceneDefaults.StartingPlacement(scene, actor, out position, out rotation, out scale);

            var geometry = new GeometryModel3D(mesh, makeMaterial(scene, actor, projectDir, brushCache));
            geometry.BackMaterial = geometry.Material;   // sony's models are not reliably wound
            geometry.Transform = new MatrixTransform3D(MakeMatrix(position, rotation, scale));
            view.Root.Children.Add(geometry);
            view.ShapeByActorId[actor.Id] = geometry;
         }
      }

      // the model's own texture where it can be decoded, plain grey otherwise. this matters more
      // than it sounds: several of Sony's parts are plain discs whose entire detail -- a clock
      // face, an eye, nostrils -- lives in the texture, so untextured they read as blank circles.
      //
      // the effect decides how it is shaded: a "pure_texture" surface ignores the lights entirely
      // and shows its picture at full brightness, so shading it here would darken things the
      // console draws bright, and the preview would disagree about the one thing it is for.
      private static Material makeMaterial(SceneProject scene, SceneActor actor, string projectDir,
                                           Dictionary<string, Brush> brushCache)
      {
         SceneMaterial material = scene.FindMaterial(actor.MaterialId);
         Brush surface = null;

         if (material != null && material.TexturePath.Length > 0)
            if (!brushCache.TryGetValue(material.Id, out surface)) {
               BitmapSource image = DdsFile.Load(resolve(projectDir, material.TexturePath));
               surface = image == null ? null : new ImageBrush(image) { ViewportUnits = BrushMappingMode.Absolute };
               if (surface != null) surface.Freeze();
               brushCache[material.Id] = surface;
            }

         if (surface == null) surface = new SolidColorBrush(Color.FromRgb(0xC8, 0xC8, 0xC8));
         return isUnlit(material) ? (Material)new EmissiveMaterial(surface) : new DiffuseMaterial(surface);
      }

      private static bool isUnlit(SceneMaterial material)
      {
         return material != null && SceneEffects.IsUnlit(material.Effect);
      }

      // scale, then rotate, then move -- the order the console applies them. one matrix rather than
      // a group of transform objects, because the player recomputes this for every object on every
      // frame and a matrix is four numbers being multiplied rather than five objects being built.
      public static Matrix3D MakeMatrix(Vec3 position, Vec3 rotation, Vec3 scale)
      {
         var matrix = Matrix3D.Identity;
         matrix.Scale(new Vector3D(scale.X, scale.Y, scale.Z));
         turn(ref matrix, new Vector3D(1, 0, 0), rotation.X);
         turn(ref matrix, new Vector3D(0, 1, 0), rotation.Y);
         turn(ref matrix, new Vector3D(0, 0, 1), rotation.Z);
         matrix.Translate(new Vector3D(position.X, position.Y, position.Z));
         return matrix;
      }

      private static void turn(ref Matrix3D matrix, Vector3D axis, double radians)
      {
         if (Math.Abs(radians) < 1e-9) return;
         matrix.Rotate(new Quaternion(axis, radians * 180.0 / Math.PI));
      }

      private static string resolve(string projectDir, string storedPath)
      {
         if (string.IsNullOrEmpty(storedPath)) return "";
         return Path.IsPathRooted(storedPath) ? storedPath : Path.Combine(projectDir, storedPath);
      }

      private static Point3D toPoint(Vec3 value) { return new Point3D(value.X, value.Y, value.Z); }
      private static Vector3D toVector(Vec3 value) { return new Vector3D(value.X, value.Y, value.Z); }

      public static Color ToColor(Vec3 value)
      {
         return Color.FromRgb(ToByte(value.X), ToByte(value.Y), ToByte(value.Z));
      }

      internal static byte ToByte(double value)
      {
         int scaled = (int)Math.Round(value * 255);
         return (byte)(scaled < 0 ? 0 : (scaled > 255 ? 255 : scaled));
      }
   }
}
