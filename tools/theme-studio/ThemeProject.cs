using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Xml;

namespace ThemeStudio
{
   // one background entry. a theme has up to 24. an entry is EITHER a flat image pair
   // (widescreen + 4:3) or a single 3D scene -- p3tcompiler rejects both at once.
   public class Background
   {
      public string WidescreenPath = "";   // 1920x1080 jpeg
      public string StandardPath = "";     // 640x480 jpeg
      public string ScenePath = "";        // an already-compiled .raf
      public bool IsProjectScene;          // or the 3D scene edited in this project
      public string From = "";             // showtype days/datetime window, optional
      public string Until = "";

      public bool IsScene { get { return IsProjectScene || !string.IsNullOrEmpty(ScenePath); } }
   }

   // the user's editable theme. this -- not the compiled .p3t -- is the source of truth,
   // because compilation is one-way and nothing can be recovered from the output.
   public class ThemeProject
   {
      public const int MaxBackgrounds = 24;

      public string Name = "my theme";
      public string Author = "";
      public string Genre = "others";
      public string Version = "version 1.0";
      public string Comment = "";
      public string Url = "";

      public string IconPath = "";         // 64x64 32-bit png
      public string AuthorIconPath = "";   // 64x64 32-bit png
      public string PreviewPath = "";      // 480x270 24-bit png
      public string NotificationPath = ""; // 64x64 32-bit png, the popup frame

      public int FontSelection;            // 0-2
      public int ColorSelection;           // 0-12, 0 = shifts with time of day
      public string BackgroundShowType = "";   // "", "days" or "datetime"

      public readonly List<Background> Backgrounds = new List<Background>();
      public readonly SceneProject Scene = new SceneProject();   // the 3D background, if any
      public readonly Dictionary<string, string> IconPaths = new Dictionary<string, string>();     // slot id -> png
      public readonly Dictionary<string, string> PointerPaths = new Dictionary<string, string>();  // slot id -> png
      public readonly Dictionary<string, string> SoundPaths = new Dictionary<string, string>();    // slot id -> vag

      // where the .themeproj file lives, once it has been saved anywhere
      public string ProjectPath = "";

      // the project unpacked: every asset the theme uses, and the scene script. stored paths are
      // relative to this. it lives in a temporary folder while the project is open, and is packed
      // back into the single .themeproj file on save.
      public string ContentDir = "";

      // the folder holding the .themeproj itself. build output goes here, beside the project,
      // rather than into the temporary content folder where it would be thrown away.
      public string ProjectFolder
      {
         get { return ProjectPath.Length == 0 ? "" : Path.GetDirectoryName(Path.GetFullPath(ProjectPath)); }
      }

      public static ThemeProject Load(string path)
      {
         // a project used to be a plain xml file pointing at assets elsewhere on the disk. those
         // still open: the paths are made absolute here, and importing brings them in on save.
         bool packaged = ProjectPackage.IsPackage(path);
         string contentDir = packaged ? ProjectPackage.Unpack(path) : ProjectPackage.MakeWorkDir();
         string detailsPath = packaged ? Path.Combine(contentDir, ProjectPackage.DetailsEntry) : path;

         var project = new ThemeProject { ProjectPath = path, ContentDir = contentDir };
         var document = new XmlDocument();
         document.Load(detailsPath);
         XmlElement root = document.DocumentElement;
         if (root == null) throw new InvalidDataException("project file is empty");

         // info
         XmlElement info = (XmlElement)root.SelectSingleNode("info");
         if (info != null) {
            project.Name = getAttribute(info, "name", project.Name);
            project.Author = getAttribute(info, "author", "");
            project.Genre = getAttribute(info, "genre", "others");
            project.Version = getAttribute(info, "version", "version 1.0");
            project.Comment = getAttribute(info, "comment", "");
            project.Url = getAttribute(info, "url", "");
            project.IconPath = getAttribute(info, "icon", "");
            project.AuthorIconPath = getAttribute(info, "authoricon", "");
            project.PreviewPath = getAttribute(info, "preview", "");
            project.NotificationPath = getAttribute(info, "notification", "");
         }

         // look and feel
         XmlElement look = (XmlElement)root.SelectSingleNode("look");
         if (look != null) {
            project.FontSelection = getIntAttribute(look, "font", 0);
            project.ColorSelection = getIntAttribute(look, "color", 0);
            project.BackgroundShowType = getAttribute(look, "showtype", "");
         }

         // backgrounds
         foreach (XmlElement element in root.SelectNodes("backgrounds/background")) {
            project.Backgrounds.Add(new Background {
               WidescreenPath = getAttribute(element, "hd", ""),
               StandardPath = getAttribute(element, "sd", ""),
               ScenePath = getAttribute(element, "scene", ""),
               IsProjectScene = getAttribute(element, "projectscene", "") == "1",
               From = getAttribute(element, "from", ""),
               Until = getAttribute(element, "until", "")
            });
         }

         SceneStorage.Read(root, project.Scene);
         project.Scene.EnsureLights();   // also names the lights of projects saved before they had fixed names

         readSlots(root, "icons/icon", project.IconPaths);
         readSlots(root, "pointers/pointer", project.PointerPaths);
         readSlots(root, "sounds/sound", project.SoundPaths);

         if (!packaged) project.rebaseOnto(Path.GetDirectoryName(Path.GetFullPath(path)));
         return project;
      }

      // an older project's paths were relative to the folder it sat in. made absolute here so the
      // rest of the editor sees one kind of path, and so saving imports them into the package.
      private void rebaseOnto(string oldFolder)
      {
         changeEveryAssetPath(stored => {
            if (stored.Length == 0 || Path.IsPathRooted(stored)) return stored;
            return Path.Combine(oldFolder, stored);
         });
      }

      // every asset path the project holds, in one place, so importing and rebasing cannot miss one
      private void changeEveryAssetPath(Func<string, string> change)
      {
         IconPath = change(IconPath);
         AuthorIconPath = change(AuthorIconPath);
         PreviewPath = change(PreviewPath);
         NotificationPath = change(NotificationPath);

         foreach (Background background in Backgrounds) {
            background.WidescreenPath = change(background.WidescreenPath);
            background.StandardPath = change(background.StandardPath);
            background.ScenePath = change(background.ScenePath);
         }

         foreach (SceneModel model in Scene.Models) model.DaePath = change(model.DaePath);
         foreach (SceneMaterial material in Scene.Materials) material.TexturePath = change(material.TexturePath);
         Scene.ScriptPath = change(Scene.ScriptPath);

         changeSlots(IconPaths, change);
         changeSlots(PointerPaths, change);
         changeSlots(SoundPaths, change);
      }

      private static void changeSlots(Dictionary<string, string> paths, Func<string, string> change)
      {
         foreach (string id in new List<string>(paths.Keys)) paths[id] = change(paths[id]);
      }

      // pulls every asset into the project, writes the details beside them, and packs the lot into
      // the single .themeproj file
      public void Save(string path)
      {
         ProjectPath = path;
         if (ContentDir.Length == 0) ContentDir = ProjectPackage.MakeWorkDir();

         importAssets();
         writeTo(Path.Combine(ContentDir, ProjectPackage.DetailsEntry));
         ProjectPackage.Pack(ContentDir, path);
      }

      // copies anything still living outside the project into it, and rewrites the path to match.
      // a file already inside is left where it is, so saving twice does not pile up copies.
      private void importAssets()
      {
         var imported = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
         changeEveryAssetPath(stored => {
            if (stored.Length == 0 || !Path.IsPathRooted(stored)) return stored;
            if (!File.Exists(stored)) return stored;   // reported as missing by the build, not lost here
            return ProjectPackage.Import(ContentDir, stored, imported);
         });
      }

      public void Close() { ProjectPackage.Discard(ContentDir); ContentDir = ""; }

      private void writeTo(string path)
      {
         var settings = new XmlWriterSettings { Indent = true, IndentChars = "   " };
         using (XmlWriter writer = XmlWriter.Create(path, settings)) {
            writer.WriteStartElement("themeproject");

            writer.WriteStartElement("info");
            writeIfSet(writer, "name", Name);
            writeIfSet(writer, "author", Author);
            writeIfSet(writer, "genre", Genre);
            writeIfSet(writer, "version", Version);
            writeIfSet(writer, "comment", Comment);
            writeIfSet(writer, "url", Url);
            writeIfSet(writer, "icon", IconPath);
            writeIfSet(writer, "authoricon", AuthorIconPath);
            writeIfSet(writer, "preview", PreviewPath);
            writeIfSet(writer, "notification", NotificationPath);
            writer.WriteEndElement();

            writer.WriteStartElement("look");
            writer.WriteAttributeString("font", FontSelection.ToString(CultureInfo.InvariantCulture));
            writer.WriteAttributeString("color", ColorSelection.ToString(CultureInfo.InvariantCulture));
            writeIfSet(writer, "showtype", BackgroundShowType);
            writer.WriteEndElement();

            writer.WriteStartElement("backgrounds");
            foreach (Background background in Backgrounds) {
               writer.WriteStartElement("background");
               writeIfSet(writer, "hd", background.WidescreenPath);
               writeIfSet(writer, "sd", background.StandardPath);
               writeIfSet(writer, "scene", background.ScenePath);
               if (background.IsProjectScene) writer.WriteAttributeString("projectscene", "1");
               writeIfSet(writer, "from", background.From);
               writeIfSet(writer, "until", background.Until);
               writer.WriteEndElement();
            }
            writer.WriteEndElement();

            SceneStorage.Write(writer, Scene);

            writeSlots(writer, "icons", "icon", IconPaths);
            writeSlots(writer, "pointers", "pointer", PointerPaths);
            writeSlots(writer, "sounds", "sound", SoundPaths);

            writer.WriteEndElement();
         }
      }

      // a stored path names a file inside the project; anything still absolute has not been
      // imported yet, and is used where it lies until the next save brings it in
      public string ResolveAsset(string storedPath)
      {
         if (string.IsNullOrEmpty(storedPath)) return "";
         if (Path.IsPathRooted(storedPath)) return storedPath;
         return ContentDir.Length == 0 ? storedPath
                                       : Path.Combine(ContentDir, storedPath.Replace('/', Path.DirectorySeparatorChar));
      }

      // the three slot tables all save and load the same way
      private void writeSlots(XmlWriter writer, string listName, string itemName,
                              Dictionary<string, string> paths)
      {
         writer.WriteStartElement(listName);
         foreach (KeyValuePair<string, string> slot in paths) {
            writer.WriteStartElement(itemName);
            writer.WriteAttributeString("id", slot.Key);
            writer.WriteAttributeString("src", slot.Value);
            writer.WriteEndElement();
         }
         writer.WriteEndElement();
      }

      private static void readSlots(XmlElement root, string path, Dictionary<string, string> paths)
      {
         foreach (XmlElement element in root.SelectNodes(path)) {
            string id = getAttribute(element, "id", "");
            string source = getAttribute(element, "src", "");
            if (id.Length > 0 && source.Length > 0) paths[id] = source;
         }
      }

      private static string getAttribute(XmlElement element, string name, string fallback)
      {
         string value = element.GetAttribute(name);
         return string.IsNullOrEmpty(value) ? fallback : value;
      }

      private static int getIntAttribute(XmlElement element, string name, int fallback)
      {
         int value;
         return int.TryParse(element.GetAttribute(name), out value) ? value : fallback;
      }

      private static void writeIfSet(XmlWriter writer, string name, string value)
      {
         if (!string.IsNullOrEmpty(value)) writer.WriteAttributeString(name, value);
      }
   }
}
