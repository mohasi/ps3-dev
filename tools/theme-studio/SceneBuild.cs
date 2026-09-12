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

      // stageDir is the theme's own build folder, beside the project, so one build leaves its work
      // in one place. it must not be the project's content folder: staging there packed every
      // intermediate into the saved .themeproj and turned a 3MB project into 59MB.
      public static SceneBuildResult Build(SceneProject scene, string contentDir, string sceneName,
                                           string stageDir, Action<string> log)
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

         // the compiler resolves every asset relative to the scene xml and writes its intermediates
         // beside it, so the whole scene build happens wherever the caller points it
         Directory.CreateDirectory(stageDir);

         string xmlPath = Path.Combine(stageDir, sceneName + ".xml");
         writeSceneXml(scene, contentDir, stageDir, xmlPath, log);
         log("staged scene to " + stageDir);

         // compile
         int exitCode;
         string output = ToolRun.Run(RafCompilerExe, "\"" + xmlPath + "\"", out exitCode);
         result.Log = output;
         result.Budget = readBudget(output);

         string producedPath = Path.ChangeExtension(xmlPath, ".raf");
         if (!File.Exists(producedPath)) {
            reportFailure(output, exitCode, stageDir, log);
            return result;
         }

         result.Succeeded = true;
         result.OutputPath = producedPath;
         if (result.Budget.WasReported) log(result.Budget.ToString());
         if (result.Budget.IsNearlyFull)
            log("warning: this scene is nearly at the console's limit -- adding much more will not build");
         warnIfTooHeavyToDraw(scene, log);
         log("built " + producedPath + " (" + new FileInfo(producedPath).Length + " bytes)");
         return result;
      }

      // the compiler's limits are all about memory, but the console also has a per-frame budget for
      // *drawing* the scene, which the compiler never checks. a stack of full-screen textured layers
      // blends over the same pixels again and again, and enough of them tips the background over that
      // budget -- on the console the theme is dropped with "RAF Error: reduce CPU load", which can
      // look like a freeze. so a scene with many drawn-over layers is flagged here, where it can still
      // be changed. this is an estimate, not the compiler's word: overlap depends on where a script
      // places things, which is not known at build time.
      private const int LayersLikelyTooHeavy = 4;

      private static void warnIfTooHeavyToDraw(SceneProject scene, Action<string> log)
      {
         int texturedLayers = 0;
         foreach (SceneActor actor in scene.Actors) {
            SceneMaterial material = scene.FindMaterial(actor.MaterialId);
            if (material != null && material.TexturePath.Length > 0) texturedLayers++;
         }
         if (texturedLayers < LayersLikelyTooHeavy) return;

         log("warning: " + texturedLayers + " textured layers may be too much for the background to draw " +
             "in time -- the console can drop a too-heavy theme (\"RAF Error: reduce CPU load\"), which looks " +
             "like a freeze. if it does, use fewer layers or lower-detail models and smaller textures.");
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

      // raf_compiler ends on "RAF Compiler Failed", which says nothing, and when it stops part way
      // through it says nothing at all: no "error:" line, and everything it had not yet written out
      // goes with it. one line of its output is never enough to tell what happened, so a failure
      // reports what it complained about, how it ended, everything it did say, and what it had
      // managed to write to disk.
      private static void reportFailure(string output, int exitCode, string stageDir, Action<string> log)
      {
         // what it complained about
         bool anyReported = false;
         double worstOverBudget = 0;
         foreach (string line in splitLines(output)) {
            string complaint = line.Trim();
            if (!complaint.StartsWith("error", StringComparison.OrdinalIgnoreCase)) continue;
            log(complaint);
            anyReported = true;
            worstOverBudget = Math.Max(worstOverBudget, getTimesOverBudget(complaint));
         }
         // a compiler that ran to the end and refused says why in its last line. one that crashed
         // stopped mid-sentence, so its last line is whatever it happened to be printing, and
         // saying how it ended is the honest answer instead.
         if (!anyReported && exitCode >= 0) log(lastMeaningfulLine(output));

         if (worstOverBudget > 1)
            log("this scene is " + worstOverBudget.ToString("0.#", CultureInfo.InvariantCulture) +
                " times the size the console allows -- a model straight out of a modelling program " +
                "usually is. reduce its triangles there and import it again.");

         // how it ended, and everything it said on the way
         log(describeExit(exitCode));
         log("raf_compiler said:");
         foreach (string line in splitLines(output))
            log("   " + line.TrimEnd());

         reportProgress(stageDir, log);
         log("scene build failed");
      }

      // a tool that dies rather than returning leaves a windows exception code behind, and that is
      // the only sign it crashed -- it writes no error line of its own. 0xC0000005 is by far the
      // most common: the program read or wrote memory it does not own.
      private static string describeExit(int exitCode)
      {
         if (exitCode == 0) return "raf_compiler reported success but wrote no .raf";
         if (exitCode == -1) return "raf_compiler did not finish";
         if (exitCode >= 0) return "raf_compiler gave up, exit code " + exitCode;

         string code = "0x" + exitCode.ToString("X8", CultureInfo.InvariantCulture);
         string kind = exitCode == unchecked((int)0xC0000005) ? " -- it used memory it does not own" : "";
         return "raf_compiler crashed (" + code + kind + "), so it stopped part way through and " +
                "whatever it had not written out yet was lost";
      }

      // a crash takes the compiler's own account of itself with it, but what it had already written
      // stays on disk, and that says how far it got: .edge is a model converted, .gtf a texture,
      // .jsx the script, .sxml the last file written before the scene is packed.
      private static void reportProgress(string stageDir, Action<string> log)
      {
         string intermediateDir = Path.Combine(stageDir, "tmp");
         if (!Directory.Exists(intermediateDir)) {
            log("it wrote nothing to " + intermediateDir + ", so it stopped before converting anything");
            return;
         }

         log("it had written these to " + intermediateDir + " before it stopped:");
         foreach (string file in Directory.GetFiles(intermediateDir))
            log("   " + Path.GetFileName(file) + " (" + new FileInfo(file).Length + " bytes)");
      }

      // "error: total geometry & script size 4111747 Bytes ( > 1048576)". the limits are the
      // compiler's own numbers rather than any written down here, so they cannot drift out of date.
      private static double getTimesOverBudget(string complaint)
      {
         Match match = Regex.Match(complaint, @"size\s+(\d+)\s+Bytes\s*\(\s*>\s*(\d+)", RegexOptions.IgnoreCase);
         double used, allowed;
         if (!match.Success ||
             !double.TryParse(match.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out used) ||
             !double.TryParse(match.Groups[2].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out allowed) ||
             allowed <= 0)
            return 0;
         return used / allowed;
      }

      // the compiler separates its steps with rows of dashes, which were being reported as the
      // reason a build failed, so a line has to carry at least one letter or digit to count
      private static string lastMeaningfulLine(string output)
      {
         string[] lines = splitLines(output);
         for (int index = lines.Length - 1; index >= 0; index--) {
            string line = lines[index].Trim();
            if (!hasWords(line)) continue;
            if (line.IndexOf("succeeded", StringComparison.OrdinalIgnoreCase) >= 0) continue;
            return line;
         }
         return "raf_compiler produced no output";
      }

      private static string[] splitLines(string output)
      {
         return output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
      }

      private static bool hasWords(string line)
      {
         foreach (char character in line)
            if (char.IsLetterOrDigit(character)) return true;
         return false;
      }

      // scene xml

      private static void writeSceneXml(SceneProject scene, string contentDir, string stageDir, string xmlPath, Action<string> log)
      {
         var settings = new XmlWriterSettings { Indent = true, IndentChars = "  " };
         using (XmlWriter writer = XmlWriter.Create(xmlPath, settings)) {
            writer.WriteStartElement("raf");

            // only what an actor actually uses is written. a model or material left in the project
            // after its object was deleted would otherwise still be compiled in -- padding the file,
            // confusing the log, and (for a material with no texture) reaching the console as
            // "Invalid texture: id=0".
            var usedModels = new HashSet<string>();
            var usedMaterials = new HashSet<string>();
            foreach (SceneActor actor in scene.Actors) {
               if (actor.ModelId.Length > 0) usedModels.Add(actor.ModelId);
               if (actor.MaterialId.Length > 0) usedMaterials.Add(actor.MaterialId);
            }

            // models, with any baked animation the .dae carries.
            //
            // several objects can point at the same shape file (importing one .dae twice for, say,
            // a day and a night layer). the console crashes on two <model> ids that share one file:
            // each is compiled into its own copy of the shape, and the two copies carry the same
            // names inside, which the console trips over. so a shared file is written as ONE model
            // that every actor using it points at -- exactly how Sony's own themes reuse geometry
            // (one <model id="mdl_bg">, eight actors). each file is staged and mended once.
            var stagedModels = new HashSet<string>();
            var modelForFile = new Dictionary<string, string>();   // staged file -> the one model written for it
            var modelIdMap = new Dictionary<string, string>();     // project model id -> the model an actor should use
            foreach (SceneModel model in scene.Models) {
               if (!usedModels.Contains(model.Id)) continue;
               string staged = stageModel(contentDir, model.DaePath, stageDir, stagedModels, log);
               if (staged.Length == 0) continue;

               string owner;
               if (!modelForFile.TryGetValue(staged, out owner)) {
                  owner = model.Id;
                  modelForFile[staged] = owner;
                  writer.WriteStartElement("model");
                  writer.WriteAttributeString("id", owner);
                  writer.WriteAttributeString("file", staged);
                  if (model.HasAnimation) {
                     writer.WriteStartElement("animation");
                     writer.WriteAttributeString("id", owner + "_anim");
                     writer.WriteAttributeString("file", staged);
                     writer.WriteEndElement();
                  }
                  writer.WriteEndElement();
               } else {
                  log("\"" + model.Id + "\" reuses the same shape as \"" + owner + "\", so they share one model");
               }
               modelIdMap[model.Id] = owner;
            }

            // materials and their textures
            foreach (SceneMaterial material in scene.Materials) {
               if (!usedMaterials.Contains(material.Id)) continue;
               writer.WriteStartElement("material");
               writer.WriteAttributeString("id", material.Id);
               writer.WriteAttributeString("effect", material.Effect);
               string staged = stage(contentDir, material.TexturePath, stageDir);
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
               string modelId;
               if (!modelIdMap.TryGetValue(actor.ModelId, out modelId)) modelId = actor.ModelId;
               writer.WriteAttributeString("model", modelId);
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
            string stagedScript = stage(contentDir, scene.ScriptPath, stageDir);
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

      // the on-disk path of an asset, whether it is stored absolute or relative to the project
      private static string findAsset(string contentDir, string storedPath)
      {
         if (string.IsNullOrEmpty(storedPath)) return "";
         string source = Path.IsPathRooted(storedPath) ? storedPath : Path.Combine(contentDir, storedPath);
         return File.Exists(source) ? source : "";
      }

      // copies an asset beside the scene xml and returns the bare filename, or "" if unusable
      private static string stage(string contentDir, string storedPath, string stageDir)
      {
         string source = findAsset(contentDir, storedPath);
         if (source.Length == 0) return "";
         string fileName = ThemeBuild.MakeStagedFileName(Path.GetFileName(source));
         File.Copy(source, Path.Combine(stageDir, fileName), true);
         return fileName;
      }

      // stages a model, mending anything the compiler would reject (Z-up, no material binding) into
      // the staged copy first. the user's own file is left alone; the mend is reported. a file
      // shared by several models is staged only once.
      private static string stageModel(string contentDir, string storedPath, string stageDir,
                                       HashSet<string> alreadyStaged, Action<string> log)
      {
         string source = findAsset(contentDir, storedPath);
         if (source.Length == 0) return "";

         string fileName = ThemeBuild.MakeStagedFileName(Path.GetFileName(source));
         if (!alreadyStaged.Add(fileName)) return fileName;

         string staged = Path.Combine(stageDir, fileName);
         try {
            DaeCompatibility.Changes changes = DaeCompatibility.WriteReady(source, staged);
            if (changes.TurnedUpright) log("turned " + fileName + " upright for the compiler (it was not Y-up)");
            if (changes.BoundMaterial) log("gave " + fileName + " a material to carry its texture (it had none)");
         } catch (Exception) {
            // an odd file the mender cannot parse: stage it as-is and let the compiler say why
            File.Copy(source, staged, true);
         }
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
