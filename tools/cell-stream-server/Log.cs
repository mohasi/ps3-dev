using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace CellStreamServer
{
   // the server has no console any more (it lives in the tray), so its running commentary goes to a
   // file. the last few lines are also kept in memory for the tray's "recent activity" view.
   internal static class Log
   {
      private const int RecentLines = 200;
      private const long MaxBytes = 2 * 1024 * 1024;

      private static readonly object Gate = new object();
      private static readonly Queue<string> Recent = new Queue<string>();
      private static string path;

      public static string Path
      {
         get
         {
            if (path == null)
            {
               string folder = System.IO.Path.Combine(
                  Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "cell-stream-server");
               Directory.CreateDirectory(folder);
               path = System.IO.Path.Combine(folder, "server.log");
            }
            return path;
         }
      }

      public static void Write(string message)
      {
         string line = "[" + DateTime.Now.ToString("HH:mm:ss.fff") + "] " + message;
         lock (Gate)
         {
            Recent.Enqueue(line);
            while (Recent.Count > RecentLines) Recent.Dequeue();
            try
            {
               if (File.Exists(Path) && new FileInfo(Path).Length > MaxBytes) File.Delete(Path);
               File.AppendAllText(Path, line + Environment.NewLine);
            }
            catch (IOException) { }   // a log we cannot write must never take the server down
         }
      }

      public static string GetRecent()
      {
         var text = new StringBuilder();
         lock (Gate) foreach (string line in Recent) text.AppendLine(line);
         return text.ToString();
      }
   }
}
