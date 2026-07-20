using System;
using System.Collections.Generic;
using System.IO;

namespace ThemeStudio
{
   // settings.txt next to the exe: key=value lines, # comments. created with documented defaults
   // on first run, and any setting added in a later version is appended to an existing file, so
   // reading the file is always enough to discover what can be configured.
   public static class AppSettings
   {
      private class Setting
      {
         public string Key;
         public string Default;
         public string[] Description;
      }

      private static readonly Setting[] Documented =
      {
         new Setting { Key = "ps3ip", Default = "", Description = new[]
         {
            "# the PS3's IP address on your network, for Deploy (uploads the built theme to",
            "# /dev_hdd0/theme over FTP, where the console lists it with no install step).",
            "# the console needs an FTP server running (e.g. simple-ftp).",
            "# empty = Deploy asks you to set it."
         } }
      };

      private static Dictionary<string, string> values;

      // the PS3's IP address for Deploy over FTP; empty = not set
      public static string Ps3Ip { get { return get("ps3ip"); } }

      public static string SettingsPath
      {
         get { return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "settings.txt"); }
      }

      // writes one setting back, so a value the user can change belongs in the window rather than
      // in a file they have to find and edit by hand
      public static void Set(string key, string value)
      {
         if (values == null) Load();
         values[key] = value;

         string[] lines = File.ReadAllLines(SettingsPath);
         for (int index = 0; index < lines.Length; index++)
            if (lines[index].TrimStart().StartsWith(key + "=")) {
               lines[index] = key + "=" + value;
               File.WriteAllLines(SettingsPath, lines);
               return;
            }
         File.AppendAllText(SettingsPath, Environment.NewLine + key + "=" + value + Environment.NewLine);
      }

      public static void Load()
      {
         if (!File.Exists(SettingsPath)) File.WriteAllLines(SettingsPath, buildDefaultFile());

         values = new Dictionary<string, string>();
         foreach (string line in File.ReadAllLines(SettingsPath)) {
            string trimmed = line.Trim();
            if (trimmed == "" || trimmed.StartsWith("#")) continue;
            int separator = trimmed.IndexOf('=');
            if (separator <= 0) continue;
            values[trimmed.Substring(0, separator).Trim()] = trimmed.Substring(separator + 1).Trim();
         }
         appendMissingSettings();
      }

      private static string get(string key)
      {
         if (values == null) Load();
         string value;
         return values.TryGetValue(key, out value) ? value : "";
      }

      private static List<string> buildDefaultFile()
      {
         var lines = new List<string> { "# theme-studio settings" };
         foreach (Setting setting in Documented) appendSetting(lines, setting);
         return lines;
      }

      // a settings.txt written by an older version is missing the newer keys; add them to the file
      // with their documentation, AND to the loaded values so the setting works this session
      private static void appendMissingSettings()
      {
         var lines = new List<string>();
         foreach (Setting setting in Documented)
            if (!values.ContainsKey(setting.Key)) {
               appendSetting(lines, setting);
               values[setting.Key] = setting.Default;
            }
         if (lines.Count == 0) return;

         try { File.AppendAllLines(SettingsPath, lines); }
         catch { }   // read-only or locked: the append is cosmetic, the value is already live
      }

      private static void appendSetting(List<string> lines, Setting setting)
      {
         lines.Add("");
         lines.AddRange(setting.Description);
         lines.Add(setting.Key + "=" + setting.Default);
      }
   }
}
