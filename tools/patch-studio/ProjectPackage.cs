using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Packaging;

namespace PatchStudio
{
   // a .patchproj is one file holding a whole project: project.txt, the dump (manifest.txt +
   // <hash>.bin), the edited/ pngs and the built patch. it is a zip written through
   // System.IO.Packaging (the library .docx uses). only this editor reads it back, so the layout
   // inside is ours. while a project is open it lives unpacked in a temp working folder; save packs
   // that folder back into the single file. (same mechanism as theme-studio.)
   public static class ProjectPackage
   {
      private const string PartType = "application/octet-stream";

      public static string MakeWorkDir()
      {
         string workDir = Path.Combine(WorkRoot, Guid.NewGuid().ToString("N"));
         Directory.CreateDirectory(workDir);
         return workDir;
      }

      private static string WorkRoot { get { return Path.Combine(Path.GetTempPath(), "patch-studio"); } }

      public static string Unpack(string packagePath)
      {
         string workDir = MakeWorkDir();
         using (Package package = Package.Open(packagePath, FileMode.Open, FileAccess.Read))
            foreach (PackagePart part in package.GetParts())
            {
               string target = Path.Combine(workDir, safeName(Uri.UnescapeDataString(part.Uri.OriginalString)));
               Directory.CreateDirectory(Path.GetDirectoryName(target));
               using (Stream from = part.GetStream(FileMode.Open, FileAccess.Read))
               using (var to = new FileStream(target, FileMode.Create, FileAccess.Write))
                  from.CopyTo(to);
            }
         return workDir;
      }

      // written to one side and swapped in, so a failure part way through can't destroy the only copy
      public static void Pack(string workDir, string packagePath)
      {
         string partial = packagePath + ".saving";
         if (File.Exists(partial)) File.Delete(partial);

         using (Package package = Package.Open(partial, FileMode.Create))
            foreach (string file in Directory.GetFiles(workDir, "*", SearchOption.AllDirectories))
            {
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

      public static void Discard(string workDir)
      {
         try { if (workDir.Length > 0 && Directory.Exists(workDir)) Directory.Delete(workDir, true); }
         catch { }   // a file still open isn't worth failing a close over; temp is cleared by windows
      }

      // an editor that crashes never reaches Discard, so its unpacked project stays on disk. a day
      // old is old enough to be certain it isn't another running copy's.
      public static void SweepLeftovers()
      {
         if (!Directory.Exists(WorkRoot)) return;
         foreach (string folder in Directory.GetDirectories(WorkRoot))
            try { if (Directory.GetLastWriteTimeUtc(folder) < DateTime.UtcNow.AddDays(-1)) Directory.Delete(folder, true); }
            catch { }
      }

      // an archive entry names its own path, so a crafted one could otherwise write outside the folder
      private static string safeName(string entryPath)
      {
         string cleaned = entryPath.Replace('/', Path.DirectorySeparatorChar);
         var parts = new List<string>();
         foreach (string part in cleaned.Split(Path.DirectorySeparatorChar))
         {
            if (part.Length == 0 || part == ".") continue;
            if (part == "..") { if (parts.Count > 0) parts.RemoveAt(parts.Count - 1); continue; }
            parts.Add(part);
         }
         return parts.Count == 0 ? "entry" : string.Join(Path.DirectorySeparatorChar.ToString(), parts.ToArray());
      }
   }
}
