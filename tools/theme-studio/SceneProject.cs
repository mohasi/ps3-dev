using System;
using System.Collections.Generic;
using System.Globalization;

namespace ThemeStudio
{
   // a 3D position/rotation/scale/colour triple. kept independent of wpf so the scene can be
   // built and tested without a window.
   public struct Vec3
   {
      public double X, Y, Z;

      public Vec3(double x, double y, double z) { X = x; Y = y; Z = z; }

      public static Vec3 One { get { return new Vec3(1, 1, 1); } }

      public override string ToString()
      {
         return format(X) + ", " + format(Y) + ", " + format(Z);
      }

      public static Vec3 Parse(string text, Vec3 fallback)
      {
         if (string.IsNullOrEmpty(text)) return fallback;
         string[] parts = text.Split(',');
         if (parts.Length != 3) return fallback;
         double x, y, z;
         if (!tryParse(parts[0], out x) || !tryParse(parts[1], out y) || !tryParse(parts[2], out z))
            return fallback;
         return new Vec3(x, y, z);
      }

      private static bool tryParse(string text, out double value)
      {
         return double.TryParse(text.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out value);
      }

      private static string format(double value)
      {
         return value.ToString("0.######", CultureInfo.InvariantCulture);
      }
   }

   public class SceneModel
   {
      public string Id = "";
      public string DaePath = "";
      public bool HasAnimation;     // the dae carries baked keyframes to declare

      // where this model asks to start, how it is turned, and how big it is drawn. read out of
      // the .dae and kept here so the preview and the object list can show the same starting
      // values the console will use.
      public Vec3 DefaultPosition = new Vec3(0, 0, 0);
      public Vec3 DefaultRotation = new Vec3(0, 0, 0);
      public Vec3 DefaultScale = Vec3.One;
      public double DrawnSize;      // its largest side once its own scale is applied

      public bool AsksToBePlaced
      {
         get
         {
            return Math.Abs(DefaultPosition.X) > 0.001 || Math.Abs(DefaultPosition.Y) > 0.001 ||
                   Math.Abs(DefaultPosition.Z) > 0.001 || Math.Abs(DefaultScale.X - 1) > 0.001 ||
                   Math.Abs(DefaultScale.Y - 1) > 0.001 || Math.Abs(DefaultScale.Z - 1) > 0.001 ||
                   Math.Abs(DefaultRotation.X) > 0.001 || Math.Abs(DefaultRotation.Y) > 0.001 ||
                   Math.Abs(DefaultRotation.Z) > 0.001;
         }
      }
   }

   public class SceneMaterial
   {
      public string Id = "";
      public string Effect = SceneEffects.Default;
      public string TexturePath = "";
   }

   // an object in the scene. it holds no placement of its own on purpose: a model already carries
   // where it starts, how it is turned and how big it is drawn, and the console uses those as the
   // object's starting values. anything set here would REPLACE them rather than adjust them, so
   // the editor leaves them alone and everything the user wants on top is done from the script,
   // where it can be read and changed.
   public class SceneActor
   {
      public string Id = "";
      public string ModelId = "";
      public string MaterialId = "";
      public Vec3 Position = new Vec3(0, 0, 0);
      public Vec3 Rotation = new Vec3(0, 0, 0);   // radians
      public Vec3 Scale = Vec3.One;

      // false while the object is left exactly as its model asks. the build then writes no
      // placement at all, so the model's own is what the console starts from.
      public bool Placed;
   }

   public class SceneLight
   {
      public string Id = "";
      public string Type = "point";               // point or ambient
      public Vec3 Color = Vec3.One;
      public Vec3 Position = new Vec3(0, 1, 0);
      // how the light fades with distance: 1 / (first + second x distance + third x distance squared).
      // a zero first number means no floor at all -- it divides by zero up close and falls to a few
      // percent a couple of units away, which reads as "my scene is black". Sony's own themes use a
      // flat constant, so this matches them.
      public Vec3 Attenuation = new Vec3(1.7, 0, 0);
      public Vec3 Direction = new Vec3(0, 0, -1);   // every light Sony compiles carries one
   }

   // material effect names are NEVER validated by any sdk tool -- a typo compiles cleanly and
   // renders broken on the console -- so the editor only ever offers this fixed list.
   public static class SceneEffects
   {
      public const string Default = "basic_lighting_edge_lit";

      // the nine the system software actually provides. no tenth: "sce01" was offered here for a
      // while and is not a real effect -- the compiler never checks the name, so it built clean and
      // rendered nothing on the console.
      public static readonly string[] All = {
         "basic_lighting_edge_lit",
         "basic_lighting",
         "basic_lighting_alpha_add",
         "pure_texture",
         "pure_texture_alpha_1_depth_0"
      };

      // sony's names say nothing to someone making a theme, so the ui shows these instead
      public static readonly string[] PlainNames = {
         "lit by the scene lights, with bright edges",
         "lit by the scene lights",
         "lit, and glows where it overlaps",
         "flat, ignores lighting",
         "flat, and drawn over whatever is behind it"
      };

      public static string ToPlainName(string effect)
      {
         for (int index = 0; index < All.Length; index++)
            if (All[index] == effect) return PlainNames[index];
         return PlainNames[0];
      }

      public static string FromPlainName(string plainName)
      {
         for (int index = 0; index < PlainNames.Length; index++)
            if (PlainNames[index] == plainName) return All[index];
         return Default;
      }

      // "No lighting. Display the texture as is." -- the documented behaviour of every
      // pure_texture effect, and the reason the preview must not shade one
      public static bool IsUnlit(string effect)
      {
         return !string.IsNullOrEmpty(effect) && effect.StartsWith("pure_texture", StringComparison.Ordinal);
      }
   }

   // a RAF scene: the animated 3D background. compiled separately from the theme, then
   // referenced by it. the console allows exactly one camera, at most two lights, one script.
   public class SceneProject
   {
      public const int MaxLights = 2;
      public const int MaxActors = 128;

      // the camera and the two lights are always there and always keep these names, so a script
      // can reach them by name without the user having to look anything up
      public const string CameraId = "camera";
      public const string MainLightId = "mainlight";
      public const string FillLightId = "filllight";

      public readonly List<SceneModel> Models = new List<SceneModel>();
      public readonly List<SceneMaterial> Materials = new List<SceneMaterial>();
      public readonly List<SceneActor> Actors = new List<SceneActor>();
      public readonly List<SceneLight> Lights = new List<SceneLight>();

      public Vec3 CameraPosition = new Vec3(0, 0, 4);
      public Vec3 CameraDirection = new Vec3(0, 0, -2);
      public Vec3 CameraUp = new Vec3(0, 1, 0);
      public double CameraFieldOfView = 0.927292;   // radians, matches sony's samples
      public double CameraNear = 0.01;
      public double CameraFar = 1000;

      public string ScriptPath = "";

      // how much of the world fits on screen this far in front of the camera. the view is a cone,
      // so this grows with distance -- sizing a new model, placing it and sizing a light marker
      // all measure against it.
      public double GetVisibleHeightAt(double distance)
      {
         if (distance < 0.1) distance = 4.0;
         return 2.0 * distance * Math.Tan(CameraFieldOfView / 2.0);
      }

      // how far in front of the camera the middle of the world is. the usual case is a camera set
      // back from an origin-centred scene, where this is simply how far back it stands.
      public double GetFocusDistance()
      {
         Vec3 forward = normalise(CameraDirection);
         double along = -(CameraPosition.X * forward.X + CameraPosition.Y * forward.Y + CameraPosition.Z * forward.Z);
         return along < 0.1 ? 4.0 : along;
      }

      public double GetVisibleHeight() { return GetVisibleHeightAt(GetFocusDistance()); }

      public static Vec3 normalise(Vec3 value)
      {
         double length = Math.Sqrt(value.X * value.X + value.Y * value.Y + value.Z * value.Z);
         if (length < 0.000001) return new Vec3(0, 0, -1);
         return new Vec3(value.X / length, value.Y / length, value.Z / length);
      }

      // the compiler accepts a scene with no light, but every lit object then renders black, so a
      // scene always carries both. a script can turn either one off; nothing else can remove them.
      public void EnsureLights()
      {
         keepLight("point", MainLightId, Vec3.One);
         keepLight("ambient", FillLightId, new Vec3(0.35, 0.35, 0.35));
      }

      // also renames the lights of projects saved before they had fixed names, so the snippets
      // and the objects list agree with what is actually in the scene
      private void keepLight(string type, string id, Vec3 colour)
      {
         SceneLight light = FindLight(type);
         if (light == null) Lights.Add(new SceneLight { Id = id, Type = type, Color = colour });
         else light.Id = id;
      }

      public SceneLight FindLight(string type)
      {
         foreach (SceneLight light in Lights) if (light.Type == type) return light;
         return null;
      }

      public SceneModel FindModel(string id)
      {
         foreach (SceneModel model in Models) if (model.Id == id) return model;
         return null;
      }

      public SceneMaterial FindMaterial(string id)
      {
         foreach (SceneMaterial material in Materials) if (material.Id == id) return material;
         return null;
      }

      // ids become xml identifiers and the console caps them at 31 characters
      public static string MakeId(string wanted, IEnumerable<string> taken)
      {
         var text = new System.Text.StringBuilder();
         foreach (char character in wanted)
            text.Append(char.IsLetterOrDigit(character) || character == '_' ? char.ToLowerInvariant(character) : '_');
         string baseId = text.ToString().Trim('_');
         if (baseId.Length == 0) baseId = "item";
         if (baseId.Length > 28) baseId = baseId.Substring(0, 28);

         var used = new List<string>(taken);
         if (!used.Contains(baseId)) return baseId;
         for (int suffix = 2; suffix < 1000; suffix++) {
            string candidate = baseId + "_" + suffix;
            if (!used.Contains(candidate)) return candidate;
         }
         return baseId;
      }
   }
}
