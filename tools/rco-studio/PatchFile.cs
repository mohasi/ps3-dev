using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Packaging;

namespace RcoStudio
{
   // .rcopatch = a zip holding only the files a modder changed, as <rcoName>/<fileName>.
   // apply copies them into the matching dump folders, so anyone can reproduce the mod
   // from their own firmware dumps without sharing Sony's files.
   public static class PatchFile
   {
      public static int Export(List<RcoJob> jobs, string patchPath, Action<string> log)
      {
         int fileCount = 0;
         using (Package package = Package.Open(patchPath, FileMode.Create))
         {
            foreach (RcoJob job in jobs)
            {
               foreach (string editedFile in ToolRunner.FindEditedFiles(job.DumpDir))
               {
                  Uri partUri = PackUriHelper.CreatePartUri(new Uri(job.Name + "/" + Path.GetFileName(editedFile), UriKind.Relative));
                  PackagePart part = package.CreatePart(partUri, "application/octet-stream", CompressionOption.Normal);
                  using (FileStream source = File.OpenRead(editedFile))
                  using (Stream target = part.GetStream())
                     CopyStream(source, target);
                  log("  packed " + job.Name + "/" + Path.GetFileName(editedFile));
                  fileCount++;
               }
            }
         }
         return fileCount;
      }

      // your own in-progress edits a patch would overwrite (as "rcoName/fileName"). apply has no
      // undo -- .pristine holds the dumped original, not your edit -- so the caller warns first.
      public static List<string> FindEditsThatWouldBeOverwritten(string patchPath)
      {
         var clobbered = new List<string>();
         using (Package package = Package.Open(patchPath, FileMode.Open, FileAccess.Read))
            foreach (PackagePart part in package.GetParts())
            {
               string rcoName, fileName;
               if (!TrySplitPart(part, out rcoName, out fileName)) continue;
               string targetFile = Path.Combine(ToolRunner.DumpsDir, rcoName, fileName);
               if (File.Exists(targetFile) && ToolRunner.IsEdited(targetFile)) clobbered.Add(rcoName + "/" + fileName);
            }
         return clobbered;
      }

      // applies onto existing dumps; rcos in the patch that are not dumped yet end up in missingRcos
      public static int Apply(string patchPath, Action<string> log, out List<string> missingRcos)
      {
         int applied = 0;
         var missing = new List<string>();
         using (Package package = Package.Open(patchPath, FileMode.Open, FileAccess.Read))
         {
            foreach (PackagePart part in package.GetParts())
            {
               string rcoName, fileName;
               if (!TrySplitPart(part, out rcoName, out fileName)) continue;

               string dumpDir = Path.Combine(ToolRunner.DumpsDir, rcoName);
               if (!File.Exists(Path.Combine(dumpDir, rcoName + ".xml")))
               {
                  if (!missing.Contains(rcoName)) missing.Add(rcoName);
                  continue;
               }

               string targetFile = Path.Combine(dumpDir, fileName);
               using (Stream source = part.GetStream())
               using (FileStream target = File.Create(targetFile))
                  CopyStream(source, target);
               log("  applied " + rcoName + "/" + fileName);
               applied++;
            }
         }
         missingRcos = missing;
         return applied;
      }

      // a patch part uri is "/rcoName/fileName" -- false for anything else
      private static bool TrySplitPart(PackagePart part, out string rcoName, out string fileName)
      {
         rcoName = fileName = null;
         string[] segments = part.Uri.OriginalString.TrimStart('/').Split('/');
         if (segments.Length != 2) return false;
         rcoName = Uri.UnescapeDataString(segments[0]);
         fileName = Uri.UnescapeDataString(segments[1]);
         return true;
      }

      private static void CopyStream(Stream source, Stream target)
      {
         var buffer = new byte[64 * 1024];
         int read;
         while ((read = source.Read(buffer, 0, buffer.Length)) > 0) target.Write(buffer, 0, read);
      }
   }
}
