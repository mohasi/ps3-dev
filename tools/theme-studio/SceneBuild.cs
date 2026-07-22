using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml;

namespace ThemeStudio
{
   // what raf_compiler reports about a scene's memory use. it prints this on every build, so
   // the editor reads it back rather than reimplementing the console's limits.
   public class SceneBudget
   {
      public double TexturePercent = -1;
      public double GeometryPercent = -1;
      public double ActorPercent = -1;

      public bool WasReported { get { return TexturePercent >= 0 || GeometryPercent >= 0 || ActorPercent >= 0; } }

      public double Worst
      {
         get { return Math.Max(TexturePercent, Math.Max(GeometryPercent, ActorPercent)); }
      }

      // 90% is not a rule of Sony's -- the compiler simply refuses past 100 -- but a theme that
      // close has no room for the next picture, and finding that out at build time is too late
      public bool IsNearlyFull { get { return Worst >= 90; } }

      public override string ToString()
      {
         return string.Format(CultureInfo.InvariantCulture,
            "textures {0:N1}%, geometry+script {1:N1}%, actors {2:N1}% of the console's limits",
            TexturePercent, GeometryPercent, ActorPercent);
      }
   }

   public class SceneBuildResult
   {
      public bool Succeeded;
      public string OutputPath = "";
      public string Log = "";
      public SceneBudget Budget = new SceneBudget();
   }

   // compiles a SceneProject into a .raf via the sdk's raf_compiler.
   public static class SceneBuild
   {
      public static string RafCompilerExe { get { return ToolRun.Find("raf_compiler.exe"); } }

      public static SceneBuildResult Build(SceneProject scene, string projectDir, string sceneName, Action<string> log)
      {
         var result = new SceneBuildResult();
         if (!File.Exists(RafCompilerExe)) {
            result.Log = "raf_compiler.exe not found at " + RafCompilerExe;
            log(result.Log);
            return result;
         }
         if (scene.Actors.Count == 0) {
            result.Log = "the scene has no actors, so there would be nothing to see";
            log(result.Log);
            return result;
         }

         // stage: the raf tools are reported to break on paths containing spaces, and the
         // compiler resolves every asset relative to the scene xml. this sits inside the theme's
         // own folder, which is named after the theme (the scene shares its name).
         string stageDir = Path.Combine(ThemeBuild.GetOutputDir(projectDir, sceneName), sceneName + "_scene");
         if (Directory.Exists(stageDir)) Directory.Delete(stageDir, true);
         Directory.CreateDirectory(stageDir);

         string xmlPath = Path.Combine(stageDir, sceneName + ".xml");
         writeSceneXml(scene, projectDir, stageDir, xmlPath);
         log("staged scene to " + stageDir);

         // compile
         int exitCode;
         string output = ToolRun.Run(RafCompilerExe, "\"" + xmlPath + "\"", out exitCode);
         result.Log = output;
         result.Budget = readBudget(output);

         string producedPath = Path.ChangeExtension(xmlPath, ".raf");
         if (!File.Exists(producedPath)) {
            log(lastMeaningfulLine(output));
            log("scene build failed");
            return result;
         }

         result.Succeeded = true;
         result.OutputPath = producedPath;
         if (result.Budget.WasReported) log(result.Budget.ToString());
         if (result.Budget.IsNearlyFull)
            log("warning: this scene is nearly at the console's limit -- adding much more will not build");
         log("built " + producedPath + " (" + new FileInfo(producedPath).Length + " bytes)");
         return result;
      }

      // "  Total texture size 9128465 Bytes ( / maximum 15728640 Bytes, 58.0% used)"
      private static SceneBudget readBudget(string output)
      {
         var budget = new SceneBudget();
         budget.TexturePercent = readPercent(output, "texture");
         budget.GeometryPercent = readPercent(output, "geometry");
         budget.ActorPercent = readPercent(output, "actor");
         return budget;
      }

      private static double readPercent(string output, string what)
      {
         Match match = Regex.Match(output, @"Total\s+" + what + @"[^\r\n]*?([\d.]+)%\s*used",
                                   RegexOptions.IgnoreCase);
         double percent;
         if (match.Success && double.TryParse(match.Groups[1].Value, NumberStyles.Float,
                                              CultureInfo.InvariantCulture, out percent))
            return percent;
         return -1;
      }

      private static string lastMeaningfulLine(string output)
      {
         string[] lines = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
         for (int index = lines.Length - 1; index >= 0; index--) {
            string line = lines[index].Trim();
            if (line.Length > 0 && line.IndexOf("succeeded", StringComparison.OrdinalIgnoreCase) < 0) return line;
         }
         return "raf_compiler produced no output";
      }

      // scene xml

      private static void writeSceneXml(SceneProject scene, string projectDir, string stageDir, string xmlPath)
      {
         var settings = new XmlWriterSettings { Indent = true, IndentChars = "  " };
         using (XmlWriter writer = XmlWriter.Create(xmlPath, settings)) {
            writer.WriteStartElement("raf");

            // models, with any baked animation the .dae carries
            foreach (SceneModel model in scene.Models) {
               string staged = stage(projectDir, model.DaePath, stageDir);
               if (staged.Length == 0) continue;
               writer.WriteStartElement("model");
               writer.WriteAttributeString("id", model.Id);
               writer.WriteAttributeString("file", staged);
               if (model.HasAnimation) {
                  writer.WriteStartElement("animation");
                  writer.WriteAttributeString("id", model.Id + "_anim");
                  writer.WriteAttributeString("file", staged);
                  writer.WriteEndElement();
               }
               writer.WriteEndElement();
            }

            // materials and their textures
            foreach (SceneMaterial material in scene.Materials) {
               writer.WriteStartElement("material");
               writer.WriteAttributeString("id", material.Id);
               writer.WriteAttributeString("effect", material.Effect);
               string staged = stage(projectDir, material.TexturePath, stageDir);
               if (staged.Length > 0) {
                  writer.WriteStartElement("texture");
                  writer.WriteAttributeString("file", staged);
                  writer.WriteEndElement();
               }
               writer.WriteEndElement();
            }

            // actors.
            //
            // position, rotation and scale are written ONLY when the object has been moved. a
            // model carries its own placement -- the compiler reads the transform out of the .dae
            // and uses it as the actor's default -- so writing these unconditionally replaces it,
            // and a sheet built to stand up 98 units wide becomes a flat 1-unit sliver seen
            // edge on. Sony's own themes set none of the three for exactly this reason.
            foreach (SceneActor actor in scene.Actors) {
               writer.WriteStartElement("actor");
               writer.WriteAttributeString("id", actor.Id);
               writer.WriteAttributeString("model", actor.ModelId);
               if (actor.MaterialId.Length > 0) writer.WriteAttributeString("material", actor.MaterialId);
               if (actor.Placed) {
                  writer.WriteAttributeString("position", actor.Position.ToString());
                  writer.WriteAttributeString("rotation", actor.Rotation.ToString());
                  writer.WriteAttributeString("scale", actor.Scale.ToString());
               }
               writer.WriteEndElement();
            }

            // camera -- exactly one is allowed
            writer.WriteStartElement("camera");
            writer.WriteAttributeString("id", "camera");
            writer.WriteAttributeString("type", "perspective");
            writeNumber(writer, "yfov", scene.CameraFieldOfView);
            writer.WriteAttributeString("ymag", "0");
            writeNumber(writer, "znear", scene.CameraNear);
            writeNumber(writer, "zfar", scene.CameraFar);
            writer.WriteAttributeString("position", scene.CameraPosition.ToString());
            writer.WriteAttributeString("direction", scene.CameraDirection.ToString());
            writer.WriteAttributeString("up", scene.CameraUp.ToString());
            writer.WriteEndElement();

            // lights -- at most two
            int lightCount = 0;
            foreach (SceneLight light in scene.Lights) {
               if (lightCount++ >= SceneProject.MaxLights) break;
               writer.WriteStartElement("light");
               writer.WriteAttributeString("id", light.Id);
               writer.WriteAttributeString("type", light.Type);
               writer.WriteAttributeString("color", light.Color.ToString());
               if (light.Type != "ambient") {
                  writer.WriteAttributeString("position", light.Position.ToString());
                  writer.WriteAttributeString("direction", light.Direction.ToString());
                  writer.WriteAttributeString("attenuation", light.Attenuation.ToString());
               }
               writer.WriteEndElement();
            }

            // script -- at most one
            string stagedScript = stage(projectDir, scene.ScriptPath, stageDir);
            if (stagedScript.Length > 0) {
               writer.WriteStartElement("script");
               writer.WriteAttributeString("file", stagedScript);
               writer.WriteEndElement();
            }

            writer.WriteEndElement();
         }
      }

      private static void writeNumber(XmlWriter writer, string name, double value)
      {
         writer.WriteAttributeString(name, value.ToString("0.######", CultureInfo.InvariantCulture));
      }

      // copies an asset beside the scene xml and returns the bare filename, or "" if unusable
      private static string stage(string projectDir, string storedPath, string stageDir)
      {
         if (string.IsNullOrEmpty(storedPath)) return "";
         string source = Path.IsPathRooted(storedPath) ? storedPath : Path.Combine(projectDir, storedPath);
         if (!File.Exists(source)) return "";
         string fileName = Path.GetFileName(source);
         File.Copy(source, Path.Combine(stageDir, fileName), true);
         return fileName;
      }

      // ids the user has already used, so a new one can avoid them
      public static IEnumerable<string> UsedIds(SceneProject scene)
      {
         foreach (SceneModel model in scene.Models) yield return model.Id;
         foreach (SceneMaterial material in scene.Materials) yield return material.Id;
         foreach (SceneActor actor in scene.Actors) yield return actor.Id;
         foreach (SceneLight light in scene.Lights) yield return light.Id;
      }
   }
}
