using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace PatchStudio
{
   // a project's contents live unpacked in a temp working folder (Folder): project.txt (name +
   // gameId), the dump pulled from the console (manifest.txt "hash fmt width height mips size" +
   // <hash>.bin), an edited/ folder and a build/ folder. it is packed into a single .patchproj file
   // on save (PackagePath); an unsaved project has an empty PackagePath. name and gameId identify
   // where a built patch deploys: patches/<gameId>/<name>/.
   public class DumpProject
   {
      public const string ProjectFile = "project.txt";

      public string Folder { get; private set; }       // the temp working folder
      public string PackagePath { get; private set; }   // the .patchproj on disk, "" until first save
      public string Name { get; set; }
      public string GameId { get; set; }
      public string BuildPath { get; set; }   // the .patch file Build writes; remembered so Deploy uploads from it
      public List<TextureItem> Textures { get; private set; }

      // true for a project auto-created by Download Dump (placeholder name): the first Save prompts for a
      // real name (and confirms the title id) via the New Project dialog before writing the .patchproj.
      public bool NeedsNaming { get; set; }

      public bool IsSaved { get { return PackagePath.Length > 0; } }

      private DumpProject()
      {
         Folder = "";
         PackagePath = "";
         Name = "";
         GameId = "";
         BuildPath = "";
         Textures = new List<TextureItem>();
      }

      // a new project starts empty in scratch; its dump is filled later by Download Dump, and it
      // isn't written to a .patchproj until the user saves.
      public static DumpProject New(string name, string gameId)
      {
         var project = new DumpProject { Folder = ProjectPackage.MakeWorkDir(), Name = name, GameId = gameId };
         project.SaveMeta();
         return project;
      }

      // open a saved .patchproj: unpack it to scratch and load its contents
      public static DumpProject Open(string packagePath)
      {
         var project = new DumpProject { Folder = ProjectPackage.Unpack(packagePath), PackagePath = packagePath };
         project.Load();
         return project;
      }

      // pack the working folder into the .patchproj (write metadata first so it goes in the zip)
      public void Save(string packagePath)
      {
         PackagePath = packagePath;
         SaveMeta();
         ProjectPackage.Pack(Folder, packagePath);
      }

      public void Close() { ProjectPackage.Discard(Folder); Folder = ""; }

      public void SaveMeta()
      {
         File.WriteAllLines(Path.Combine(Folder, ProjectFile), new[]
         {
            "# patch-studio project",
            "name=" + Name,
            "gameId=" + GameId,
            "buildPath=" + BuildPath
         });
      }

      public void Load()
      {
         LoadMeta();
         Textures.Clear();
         string manifest = Path.Combine(Folder, "manifest.txt");
         if (!File.Exists(manifest)) return;

         foreach (string raw in File.ReadAllLines(manifest))
         {
            string line = raw.Trim();
            if (line == "" || line.StartsWith("#")) continue;
            string[] token = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (token.Length < 6) continue;

            var item = new TextureItem
            {
               Hash   = token[0],
               Format = ParseHex(token[1]),
               Width  = ParseInt(token[2]),
               Height = ParseInt(token[3]),
               Mips   = ParseInt(token[4]),
               Size   = ParseInt(token[5]),
               BinPath = Path.Combine(Folder, token[0] + ".bin")
            };
            item.Original = Decode(item);
            item.Thumbnail = item.Original;   // Recheck swaps in the edited image for textures that were changed
            Textures.Add(item);
         }
      }

      // name/gameId from project.txt (always present in a project we wrote; the fallbacks only guard
      // against a hand-edited package)
      private void LoadMeta()
      {
         if (Name == "") Name = "Untitled";
         string file = Path.Combine(Folder, ProjectFile);
         if (!File.Exists(file)) return;
         foreach (string raw in File.ReadAllLines(file))
         {
            string line = raw.Trim();
            if (line == "" || line.StartsWith("#")) continue;
            int separator = line.IndexOf('=');
            if (separator <= 0) continue;
            string key = line.Substring(0, separator).Trim(), value = line.Substring(separator + 1).Trim();
            if (key == "name") Name = value;
            else if (key == "gameId") GameId = value;
            else if (key == "buildPath") BuildPath = value;
         }
      }

      // decode a texture's base level to a WPF image; null (caller shows a placeholder) on any failure.
      public static BitmapSource Decode(TextureItem item)
      {
         try
         {
            if (!File.Exists(item.BinPath) || !Dxt.CanDecode(item.Format)) return null;
            byte[] bin = File.ReadAllBytes(item.BinPath);
            byte[] bgra = Dxt.Decode(bin, item.Width, item.Height, item.Format);
            if (bgra == null) return null;
            var bmp = BitmapSource.Create(item.Width, item.Height, 96, 96, PixelFormats.Bgra32, null, bgra, item.Width * 4);
            bmp.Freeze();
            return bmp;
         }
         catch { return null; }
      }

      private static int ParseHex(string s) { int v; return int.TryParse(s, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v) ? v : 0; }
      private static int ParseInt(string s) { int v; return int.TryParse(s, out v) ? v : 0; }
   }
}
