using System;
using System.Collections.Generic;
using System.IO;

namespace PatchStudio
{
   // settings.txt next to the exe: key=value lines, # comments. created with documented defaults
   // on first run; keys added in a later version are appended to an existing file so users can
   // always discover what's available by reading it. (same mechanism as rco-studio.)
   public static class AppSettings
   {
      private class Setting { public string Key; public string Default; public string[] Description; }

      private static readonly Setting[] Documented =
      {
         new Setting { Key = "imageEditor", Default = @"C:\Program Files\Paint.NET\paintdotnet.exe", Description = new[]
         {
            "# full path to the image editor opened when you double-click a texture.",
            "# empty or not installed = double-click falls back to the Windows default for .png."
         } },
         new Setting { Key = "ps3ip", Default = "192.168.2.35", Description = new[]
         {
            "# the PS3's IP address, for Download Dump and Deploy over FTP.",
            "# 192.168.2.35 on WiFi, 10.0.0.2 on wired LAN. the console needs an FTP server",
            "# running (e.g. simple-ftp / webMAN). empty = those actions ask you to set it."
         } }
      };

      private static Dictionary<string, string> values;

      public static string ImageEditor { get { return Get("imageEditor"); } }
      public static string Ps3Ip { get { return Get("ps3ip"); } }

      public static void Load()
      {
         string settingsFile = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "settings.txt");
         if (!File.Exists(settingsFile)) File.WriteAllLines(settingsFile, BuildDefaultFile());

         values = new Dictionary<string, string>();
         foreach (string line in File.ReadAllLines(settingsFile))
         {
            string trimmed = line.Trim();
            if (trimmed == "" || trimmed.StartsWith("#")) continue;
            int separator = trimmed.IndexOf('=');
            if (separator <= 0) continue;
            values[trimmed.Substring(0, separator).Trim()] = trimmed.Substring(separator + 1).Trim();
         }

         AppendMissingSettings(settingsFile, values);
      }

      private static string Get(string key)
      {
         if (values == null) Load();
         string value;
         return values.TryGetValue(key, out value) ? value : "";
      }

      private static List<string> BuildDefaultFile()
      {
         var lines = new List<string> { "# patch-studio settings" };
         foreach (Setting setting in Documented) AppendSetting(lines, setting);
         return lines;
      }

      private static void AppendMissingSettings(string settingsFile, Dictionary<string, string> loaded)
      {
         var lines = new List<string>();
         foreach (Setting setting in Documented)
            if (!loaded.ContainsKey(setting.Key)) { AppendSetting(lines, setting); loaded[setting.Key] = setting.Default; }
         if (lines.Count == 0) return;
         try { File.AppendAllLines(settingsFile, lines); } catch { }
      }

      private static void AppendSetting(List<string> lines, Setting setting)
      {
         lines.Add("");
         lines.AddRange(setting.Description);
         lines.Add(setting.Key + "=" + setting.Default);
      }
   }
}
