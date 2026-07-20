using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Xml;

namespace ThemeStudio
{
   // turns a ThemeProject into a .p3t by generating p3tcompiler's input xml and running it.
   public static class ThemeBuild
   {
      // a scratch place beside the program, for work that is not a theme (checking a script)
      public static string OutputDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "built");

      // a finished theme goes in its own folder, named after the theme, beside the project it was
      // made from. one folder per theme, so several projects sharing a folder do not overwrite each
      // other -- which a single shared "built" folder did.
      public static string GetOutputDir(string projectDir, string themeFolderName)
      {
         string parent = projectDir.Length > 0 ? projectDir : AppDomain.CurrentDomain.BaseDirectory;
         return Path.Combine(parent, themeFolderName);
      }

      public static string ThemeCompilerExe { get { return ToolRun.Find("p3tcompiler.exe"); } }

      public class BuildResult
      {
         public bool Succeeded;
         public string OutputPath = "";
         public string Log = "";
      }

      public static BuildResult Build(ThemeProject project, Action<string> log)
      {
         var result = new BuildResult();
         buildLog = log;
         if (!File.Exists(ThemeCompilerExe)) {
            result.Log = "p3tcompiler.exe not found at " + ThemeCompilerExe;
            log(result.Log);
            return result;
         }

         // checked before anything is deleted or written: the compilers accept a missing asset by
         // leaving its attribute out, so a moved folder would otherwise build an empty theme
         List<string> missing = findMissingAssets(project);
         if (missing.Count > 0) {
            log("nothing was built -- " + missing.Count + (missing.Count == 1 ? " file is" : " files are") +
                " missing:");
            foreach (string entry in missing) log("   " + entry);
            result.Log = "missing assets";
            return result;
         }

         stagedNameBySource.Clear();   // names are unique within one build, not across builds
         stagedNamesUsed.Clear();

         // stage: p3tcompiler resolves asset paths relative to the xml, and the raf tools are
         // reported to break on paths containing spaces -- so build in a private space-free dir.
         // the theme's own folder is where both the staged files and the finished .p3t go.
         string themeFolder = MakeFileName(project.Name);
         string outputDir = GetOutputDir(project.ProjectFolder, themeFolder);
         string stageDir = outputDir;
         prepareDirectory(stageDir);

         // the 3D background, if the project has one, must be compiled before the theme
         string projectScenePath = "";
         if (usesProjectScene(project)) {
            SceneBuildResult scene = SceneBuild.Build(project.Scene, project.ContentDir,
                                                      MakeFileName(project.Name), log);
            if (!scene.Succeeded) {
               result.Log = "the 3D scene failed to build, so the theme was not built";
               log(result.Log);
               return result;
            }
            projectScenePath = scene.OutputPath;
         }

         string xmlPath = Path.Combine(stageDir, "theme.xml");
         writeThemeXml(project, stageDir, xmlPath, projectScenePath);
         log("staged to " + stageDir);

         // compile
         int exitCode;
         string output = ToolRun.Run(ThemeCompilerExe, "\"" + xmlPath + "\"", out exitCode);
         result.Log = output;
         log(output.Trim());

         string producedPath = Path.ChangeExtension(xmlPath, ".p3t");
         if (exitCode != 0 || !File.Exists(producedPath)) {
            log("build failed (exit " + exitCode + ")");
            return result;
         }

         // name the output after the theme so several themes can coexist on the console
         string finalPath = Path.Combine(outputDir, themeFolder + ".p3t");
         File.Copy(producedPath, finalPath, true);
         result.Succeeded = true;
         result.OutputPath = finalPath;
         log("built " + finalPath + " (" + new FileInfo(finalPath).Length + " bytes)");
         return result;
      }

      // the scene edited in this project, which has to be compiled before the theme
      private static bool usesProjectScene(ThemeProject project)
      {
         foreach (Background background in project.Backgrounds)
            if (background.IsProjectScene) return true;
         return false;
      }


      // writes p3tcompiler's input xml, copying every referenced asset in beside it
      private static void writeThemeXml(ThemeProject project, string stageDir, string xmlPath,
                                        string projectScenePath)
      {
         var settings = new XmlWriterSettings { Indent = true, IndentChars = "\t" };
         using (XmlWriter writer = XmlWriter.Create(xmlPath, settings)) {
            writer.WriteStartElement("theme");

            // theme details
            writer.WriteStartElement("infotable");
            writer.WriteStartElement("info");
            writer.WriteAttributeString("name", project.Name);
            writeIfSet(writer, "author", project.Author);
            writeIfSet(writer, "genre", project.Genre);
            writeIfSet(writer, "version", project.Version);
            writeIfSet(writer, "comment", project.Comment);
            writeIfSet(writer, "url", project.Url);
            writeIfSet(writer, "icon", stageAsset(project, project.IconPath, stageDir));
            writeIfSet(writer, "authoricon", stageAsset(project, project.AuthorIconPath, stageDir));
            writeIfSet(writer, "preview", stageAsset(project, project.PreviewPath, stageDir));
            writer.WriteEndElement();

            // the console falls back to <info> for any locale we do not name, so an english
            // entry is enough until the editor offers translations.
            writer.WriteStartElement("localizedinfo");
            writer.WriteAttributeString("locale", "en");
            writer.WriteAttributeString("name", project.Name);
            writer.WriteEndElement();
            writer.WriteEndElement();

            // xmb icons
            if (project.IconPaths.Count > 0) {
               writer.WriteStartElement("icontable");
               foreach (KeyValuePair<string, string> icon in project.IconPaths) {
                  string staged = stageAsset(project, icon.Value, stageDir);
                  if (staged.Length == 0) continue;
                  writer.WriteStartElement("icon");
                  writer.WriteAttributeString("id", icon.Key);
                  writer.WriteAttributeString("src", staged);
                  writer.WriteEndElement();
               }
               writer.WriteEndElement();
            }

            // mouse pointers
            if (project.PointerPaths.Count > 0) {
               writer.WriteStartElement("pointertable");
               foreach (PointerSlot slot in PointerSlots.All) {
                  string stored;
                  if (!project.PointerPaths.TryGetValue(slot.Id, out stored)) continue;
                  string staged = stageAsset(project, stored, stageDir);
                  if (staged.Length == 0) continue;
                  writer.WriteStartElement("pointer");
                  writer.WriteAttributeString("id", slot.Id);
                  writer.WriteAttributeString("src", staged);
                  writer.WriteAttributeString("base_x", slot.ClickX.ToString(CultureInfo.InvariantCulture));
                  writer.WriteAttributeString("base_y", slot.ClickY.ToString(CultureInfo.InvariantCulture));
                  writer.WriteEndElement();
               }
               writer.WriteEndElement();
            }

            // the notification popup frame
            string notification = stageAsset(project, project.NotificationPath, stageDir);
            if (notification.Length > 0) {
               writer.WriteStartElement("notification");
               writer.WriteAttributeString("src", notification);
               writer.WriteEndElement();
            }

            // backgrounds
            // while the project's own scene is on, the Backgrounds stage hides the still pictures,
            // so building them would ship what the user cannot see. they stay in the project file.
            // a separately-compiled .raf is a visible row, so it builds alongside pictures.
            bool sceneIsTheTheme = usesProjectScene(project);
            int skipped = 0, written = 0;

            writer.WriteStartElement("bgimagetable");
            writeIfSet(writer, "showtype", project.BackgroundShowType);
            foreach (Background background in project.Backgrounds) {
               if (sceneIsTheTheme && !background.IsProjectScene) { skipped++; continue; }
               if (written >= ThemeProject.MaxBackgrounds) { skipped++; continue; }
               written++;
               writer.WriteStartElement("bgimage");
               if (background.IsScene) {
                  string scenePath = background.IsProjectScene ? projectScenePath : background.ScenePath;
                  writer.WriteAttributeString("anim", stageAsset(project, scenePath, stageDir));
               } else {
                  writeIfSet(writer, "hd", stageAsset(project, background.WidescreenPath, stageDir));
                  writeIfSet(writer, "sd", stageAsset(project, background.StandardPath, stageDir));
               }
               writeIfSet(writer, "from", background.From);
               writeIfSet(writer, "until", background.Until);
               writer.WriteEndElement();
            }
            writer.WriteEndElement();
            if (skipped > 0)
               buildLog(sceneIsTheTheme
                  ? "note: " + skipped + " still picture(s) left out, because this theme uses a 3D scene"
                  : "note: " + skipped + " background(s) left out -- a theme holds at most " +
                    ThemeProject.MaxBackgrounds);

            // font and colour
            writeSelection(writer, "font", project.FontSelection);
            writeSelection(writer, "color", project.ColorSelection);

            // sound effects. one file per sound, named with "src" -- writing the same file as both
            // left and right would make it a stereo pair and count twice against the console's
            // 256KB budget for no benefit at all.
            if (project.SoundPaths.Count > 0) {
               writer.WriteStartElement("setable");
               long soundBytes = 0;
               foreach (SoundSlot slot in SoundSlots.All) {
                  string stored;
                  if (!project.SoundPaths.TryGetValue(slot.Id, out stored)) continue;
                  string staged = stageSound(project, stored, stageDir);
                  if (staged.Length == 0) continue;
                  soundBytes += new FileInfo(Path.Combine(stageDir, staged)).Length;
                  writer.WriteStartElement("se");
                  writer.WriteAttributeString("id", slot.Id);
                  writer.WriteAttributeString("src", staged);
                  writer.WriteEndElement();
               }
               warnIfSoundsTooBig(soundBytes);
               writer.WriteEndElement();
            }

            writer.WriteEndElement();
         }
      }

      // the console rejects a theme whose sounds add up to more than this, and says only that they
      // are too big -- so the count is reported here, where it can name the number
      private const long MostSoundBytes = 256 * 1024;

      private static void warnIfSoundsTooBig(long soundBytes)
      {
         if (soundBytes <= MostSoundBytes) return;
         buildLog("warning: the menu sounds come to " + (soundBytes / 1024) + "KB and the console allows " +
                  (MostSoundBytes / 1024) + "KB -- use shorter sounds, or fewer of them");
      }

      // a sound is converted on its way in when it is a wav; a vag is staged as it is
      private static string stageSound(ThemeProject project, string storedPath, string stageDir)
      {
         string source = project.ResolveAsset(storedPath);
         if (source.Length == 0 || !File.Exists(source)) return "";
         if (!SoundConvert.IsWav(source)) return stageAsset(project, storedPath, stageDir);
         return Path.GetFileName(SoundConvert.ToVag(source, stageDir, buildLog));
      }

      // where a conversion reports what it did, set for the length of one build
      private static Action<string> buildLog = delegate { };

      // every file the build will reach for, named the way the user chose it
      private static List<string> findMissingAssets(ThemeProject project)
      {
         var missing = new List<string>();
         checkAsset(project, project.IconPath, "theme icon", missing);
         checkAsset(project, project.AuthorIconPath, "author icon", missing);
         checkAsset(project, project.PreviewPath, "preview picture", missing);
         checkAsset(project, project.NotificationPath, "notification frame", missing);

         int number = 0;
         foreach (Background background in project.Backgrounds) {
            number++;
            if (background.IsProjectScene) continue;   // built during this run, not read from disk
            checkAsset(project, background.WidescreenPath, "background " + number, missing);
            checkAsset(project, background.StandardPath, "background " + number + " (4:3)", missing);
            checkAsset(project, background.ScenePath, "background " + number + " (3D scene)", missing);
         }

         foreach (KeyValuePair<string, string> slot in project.IconPaths)
            checkAsset(project, slot.Value, "icon " + slot.Key, missing);
         foreach (KeyValuePair<string, string> slot in project.PointerPaths)
            checkAsset(project, slot.Value, "pointer " + slot.Key, missing);
         foreach (KeyValuePair<string, string> slot in project.SoundPaths)
            checkAsset(project, slot.Value, "sound " + slot.Key, missing);

         if (usesProjectScene(project)) {
            foreach (SceneModel model in project.Scene.Models)
               checkAsset(project, model.DaePath, "model \"" + model.Id + "\"", missing);
            foreach (SceneMaterial material in project.Scene.Materials)
               checkAsset(project, material.TexturePath, "texture for \"" + material.Id + "\"", missing);
            checkAsset(project, project.Scene.ScriptPath, "scene script", missing);
         }
         return missing;
      }

      private static void checkAsset(ThemeProject project, string storedPath, string what,
                                     List<string> missing)
      {
         if (string.IsNullOrEmpty(storedPath)) return;
         if (!File.Exists(project.ResolveAsset(storedPath))) missing.Add(what + " -- " + storedPath);
      }

      // copies one asset into the staging dir and returns the bare filename to reference it by;
      // "" when unset or missing, so the caller can omit the attribute. the dir is flat, so two
      // different files named icon.png would overwrite each other -- the second gets a number.
      private static readonly Dictionary<string, string> stagedNameBySource =
         new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
      private static readonly HashSet<string> stagedNamesUsed =
         new HashSet<string>(StringComparer.OrdinalIgnoreCase);

      private static string stageAsset(ThemeProject project, string storedPath, string stageDir)
      {
         string source = project.ResolveAsset(storedPath);
         if (source.Length == 0 || !File.Exists(source)) return "";

         string alreadyStaged;
         if (stagedNameBySource.TryGetValue(source, out alreadyStaged)) return alreadyStaged;

         string fileName = makeFreeStagedName(Path.GetFileName(source));
         File.Copy(source, Path.Combine(stageDir, fileName), true);
         stagedNameBySource[source] = fileName;
         stagedNamesUsed.Add(fileName);
         return fileName;
      }

      private static string makeFreeStagedName(string wanted)
      {
         if (!stagedNamesUsed.Contains(wanted)) return wanted;
         string stem = Path.GetFileNameWithoutExtension(wanted);
         string extension = Path.GetExtension(wanted);
         for (int suffix = 2; ; suffix++) {
            string candidate = stem + "_" + suffix + extension;
            if (!stagedNamesUsed.Contains(candidate)) return candidate;
         }
      }

      private static void prepareDirectory(string path)
      {
         if (Directory.Exists(path)) Directory.Delete(path, true);
         Directory.CreateDirectory(path);
      }

      private static void writeIfSet(XmlWriter writer, string name, string value)
      {
         if (!string.IsNullOrEmpty(value)) writer.WriteAttributeString(name, value);
      }

      private static void writeSelection(XmlWriter writer, string element, int selection)
      {
         writer.WriteStartElement(element);
         writer.WriteAttributeString("selection", selection.ToString(CultureInfo.InvariantCulture));
         writer.WriteEndElement();
      }

      // turns a theme or scene name into something safe to use as a file or folder name.
      // shared, because build output, scene output and imported projects all need it.
      public static string MakeFileName(string name)
      {
         var text = new StringBuilder();
         foreach (char character in name)
            text.Append(char.IsLetterOrDigit(character) ? character : '_');
         return text.Length == 0 ? "theme" : text.ToString();
      }
   }
}
