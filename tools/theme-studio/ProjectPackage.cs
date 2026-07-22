using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Packaging;

namespace ThemeStudio
{
   // a .themeproj is one file holding everything: the theme's details, the scene script, and a
   // copy of every model, texture and sound it uses. the same idea as a .p3t, a .docx or an .odt.
   //
   // it used to be a small xml file pointing at whatever was on disk, which quietly did not work:
   // the paths were absolute, so a project could not be moved or sent anywhere, and nothing said
   // so until a build failed or a texture went missing.
   //
   // it is a zip, written through System.IO.Packaging (the same library .docx uses) rather than
   // System.IO.Compression, so the project keeps to .NET 4.0 and opens in the same VS as the other
   // tools. only this editor ever reads a .themeproj back, so the exact layout inside is ours.
   public static class ProjectPackage
   {
      public const string DetailsEntry = "project.xml";
      public const string AssetFolder = "assets";
      private const string PartType = "application/octet-stream";

      // a project is unpacked while it is open, and the copy on disk is rewritten when it is saved
      public static string Unpack(string packagePath)
      {
         string workDir = MakeWorkDir();
         using (Package package = Package.Open(packagePath, FileMode.Open, FileAccess.Read))
            foreach (PackagePart part in package.GetParts()) {
               string target = Path.Combine(workDir, safeName(Uri.UnescapeDataString(part.Uri.OriginalString)));
               Directory.CreateDirectory(Path.GetDirectoryName(target));
               using (Stream from = part.GetStream(FileMode.Open, FileAccess.Read))
               using (var to = new FileStream(target, FileMode.Create, FileAccess.Write))
                  from.CopyTo(to);
            }
         return workDir;
      }

      public static string MakeWorkDir()
      {
         string workDir = Path.Combine(WorkRoot, Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(workDir);
         return workDir;
      }

      private static string WorkRoot { get { return Path.Combine(Path.GetTempPath(), "theme-studio"); } }

      // an editor that is killed, or crashes, never reaches Discard, so its unpacked project stays
      // on disk for ever -- they had grown to 153MB before this existed. a day old is old enough
      // to be certain it does not belong to another copy of the editor running right now.
      public static void SweepLeftovers()
      {
         if (!Directory.Exists(WorkRoot)) return;
         foreach (string folder in Directory.GetDirectories(WorkRoot)) {
            try {
               if (Directory.GetLastWriteTimeUtc(folder) < DateTime.UtcNow.AddDays(-1))
                  Directory.Delete(folder, true);
            } catch (Exception) {
               // in use, or not ours to delete. it will be tried again next time.
            }
         }
      }

      // written to one side and swapped in, so a failure part way through cannot destroy the
      // only copy of someone's work
      public static void Pack(string workDir, string packagePath)
      {
         string partial = packagePath + ".saving";
         if (File.Exists(partial)) File.Delete(partial);

         using (Package package = Package.Open(partial, FileMode.Create))
            foreach (string file in Directory.GetFiles(workDir, "*", SearchOption.AllDirectories)) {
               string name = file.Substring(workDir.Length).TrimStart(Path.DirectorySeparatorChar)
                                 .Replace(Path.DirectorySeparatorChar, '/');
               Uri partUri = PackUriHelper.CreatePartUri(new Uri(name, UriKind.Relative));
               PackagePart part = package.CreatePart(partUri, PartType, CompressionOption.Normal);
               using (var from = new FileStream(file, FileMode.Open, FileAccess.Read))
               using (Stream to = part.GetStream())
                  from.CopyTo(to);
            }

         if (File.Exists(packagePath)) File.Replace(partial, packagePath, null);
         else File.Move(partial, packagePath);
      }

      public static bool IsPackage(string path)
      {
         try {
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Read))
               return file.ReadByte() == 'P' && file.ReadByte() == 'K';
         } catch (Exception) {
            return false;
         }
      }

      // brings an outside file into the project, under a name that will not collide with an
      // unrelated file that happens to share it
      public static string Import(string workDir, string fullPath, Dictionary<string, string> alreadyImported)
      {
         string existing;
         if (alreadyImported.TryGetValue(fullPath, out existing)) return existing;

         string folder = Path.Combine(workDir, AssetFolder);
         Directory.CreateDirectory(folder);

         string wanted = Path.GetFileName(fullPath);
         string name = wanted;
         for (int attempt = 2; File.Exists(Path.Combine(folder, name)); attempt++)
            name = Path.GetFileNameWithoutExtension(wanted) + "-" + attempt + Path.GetExtension(wanted);

         File.Copy(fullPath, Path.Combine(folder, name), true);
         string stored = AssetFolder + "/" + name;
         alreadyImported[fullPath] = stored;
         return stored;
      }

      public static void Discard(string workDir)
      {
         try {
            if (workDir.Length > 0 && Directory.Exists(workDir)) Directory.Delete(workDir, true);
         } catch (Exception) {
            // a file still open somewhere is not worth failing a close over; the temp folder goes
            // when windows clears it
         }
      }

      // an archive entry names its own path, so a crafted one could otherwise be written outside
      // the folder it is supposed to land in
      private static string safeName(string entryPath)
      {
         string cleaned = entryPath.Replace('/', Path.DirectorySeparatorChar);
         var parts = new List<string>();
         foreach (string part in cleaned.Split(Path.DirectorySeparatorChar)) {
            if (part.Length == 0 || part == ".") continue;
            if (part == "..") { if (parts.Count > 0) parts.RemoveAt(parts.Count - 1); continue; }
            parts.Add(part);
         }
         return parts.Count == 0 ? "entry" : string.Join(Path.DirectorySeparatorChar.ToString(), parts.ToArray());
      }
   }
}
