using System;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace ThemeStudio
{
   // checks a scene script by running Sony's own PSJS compiler over it. that is the only
   // authoritative answer -- the language is not javascript, so no other parser would agree.
   public static class PsjsCheck
   {
      public static string ScriptCompilerExe { get { return ToolRun.Find("raf_script.exe"); } }

      // true when the script compiles. message carries the compiler's complaint if not.
      public static bool TryCompile(string scriptPath, out string message)
      {
         message = "";
         if (!File.Exists(ScriptCompilerExe)) { message = "raf_script.exe not found"; return false; }
         if (!File.Exists(scriptPath)) { message = "script not found"; return false; }

         // compile a copy: raf_script writes its output beside the input, and the project
         // folder should not collect build leftovers.
         string workDir = Path.Combine(ThemeBuild.OutputDir, "scriptcheck");
         if (Directory.Exists(workDir)) Directory.Delete(workDir, true);
         Directory.CreateDirectory(workDir);

         string copyPath = Path.Combine(workDir, Path.GetFileName(scriptPath));
         File.Copy(scriptPath, copyPath, true);

         int exitCode;
         string output = ToolRun.Run(ScriptCompilerExe, "\"" + copyPath + "\"", out exitCode);
         string compiledPath = Path.ChangeExtension(copyPath, ".jsx");
         if (File.Exists(compiledPath)) return true;

         // the compiler names the copy it was handed; the user only knows their own file
         message = firstComplaint(output).Replace(copyPath, Path.GetFileName(scriptPath));
         return false;
      }

      // the compiler prints a banner first; the useful line is the one mentioning an error
      private static string firstComplaint(string output)
      {
         foreach (string line in output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries)) {
            string trimmed = line.Trim();
            if (trimmed.IndexOf("error", StringComparison.OrdinalIgnoreCase) >= 0 ||
                trimmed.IndexOf("syntax", StringComparison.OrdinalIgnoreCase) >= 0)
               return trimmed;
         }
         return output.Trim().Length > 0 ? output.Trim() : "the script did not compile";
      }
   }
}
