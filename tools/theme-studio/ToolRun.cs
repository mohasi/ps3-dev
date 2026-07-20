using System;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace ThemeStudio
{
   // runs one of the bundled SDK tools. three non-obvious requirements, all load-bearing:
   // the working directory must be the tool's own folder (they load sibling dlls), stdin must be
   // closed (p3tcompiler waits on an Enter keypress), and both output streams must be drained at
   // once (reading one to the end deadlocks when the other fills its few-KB pipe buffer).
   public static class ToolRun
   {
      // bundled, so theme-studio works on a machine with no SDK installed
      public static string ToolsDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "tools");

      private const int MostSeconds = 300;

      public static string Find(string exeName) { return Path.Combine(ToolsDir, exeName); }

      public static string Run(string exe, string arguments, out int exitCode)
      {
         var settings = new ProcessStartInfo(exe, arguments) {
            WorkingDirectory = Path.GetDirectoryName(exe),
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
         };

         var said = new StringBuilder();
         using (Process process = Process.Start(settings)) {
            process.OutputDataReceived += (sender, line) => appendLine(said, line.Data);
            process.ErrorDataReceived += (sender, line) => appendLine(said, line.Data);
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.StandardInput.Close();

            if (!process.WaitForExit(MostSeconds * 1000)) {
               try { process.Kill(); } catch { }
               exitCode = -1;
               return said + Environment.NewLine + Path.GetFileName(exe) + " gave up after " + MostSeconds + " seconds";
            }
            exitCode = process.ExitCode;
         }
         return said.ToString();
      }

      private static void appendLine(StringBuilder said, string line)
      {
         if (line == null) return;
         lock (said) said.AppendLine(line);
      }
   }
}
