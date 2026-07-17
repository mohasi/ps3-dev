using System;
using System.Collections.Generic;
using System.IO;

namespace RcoStudio
{
   // settings.txt next to the exe: key=value lines, # comments. created with documented
   // defaults on first run, and any setting added in a later version is appended to an
   // existing file, so users can always discover what is available by reading it.
   public static class AppSettings
   {
      private class Setting
      {
         public string Key;
         public string Default;
         public string[] Description;
      }

      // defaults point at popular tools in their usual install location. a default that is not
      // installed simply greys its menu entry out, so guessing costs nothing and saves the
      // many users who do have these from configuring anything at all.
      private static readonly Setting[] Documented =
      {
         new Setting { Key = "imageEditor", Default = @"C:\Program Files\Paint.NET\paintdotnet.exe", Description = new[]
         {
            "# full path to the image editor opened by right-click > Edit on an image.",
            "# empty or not installed = that menu entry is greyed out (double-click still opens",
            "# the windows default)."
         } },
         new Setting { Key = "textEditor", Default = @"C:\Program Files\Notepad++\notepad++.exe", Description = new[]
         {
            "# full path to the text editor opened by right-click > Edit on an xml or txt file.",
            "# empty or not installed = that menu entry is greyed out (double-click still opens",
            "# the windows default)."
         } },
         new Setting { Key = "diffTool", Default = @"C:\Program Files\WinMerge\WinMergeU.exe", Description = new[]
         {
            "# full path to a compare tool. used by right-click > Compare with dumped, which opens",
            "# it with the dumped copy on the left and your edited file on the right.",
            "# empty or not installed = that menu entry is greyed out."
         } },
         new Setting { Key = "ps3ip", Default = "", Description = new[]
         {
            "# the PS3's IP address on your network, for Deploy (uploads compiled RCOs to",
            "# dev_blind over FTP). the console needs an FTP server running (e.g. simple-ftp).",
            "# empty = Deploy asks you to set it."
         } }
      };

      private static Dictionary<string, string> values;

      // full path to an image editor exe; empty = no configured editor
      public static string ImageEditor { get { return Get("imageEditor"); } }

      // full path to a text editor exe; empty = no configured editor
      public static string TextEditor { get { return Get("textEditor"); } }

      // full path to a compare tool exe (WinMerge and friends); empty = no configured tool
      public static string DiffTool { get { return Get("diffTool"); } }

      // the PS3's IP address for Deploy over FTP; empty = not set
      public static string Ps3Ip { get { return Get("ps3ip"); } }

      // creates settings.txt with documented defaults, or appends any setting added since it
      // was written. call once at startup, so the file describes the current version whether
      // or not the user ever opens a menu that reads a setting.
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
         var lines = new List<string> { "# rco-studio settings" };
         foreach (Setting setting in Documented) AppendSetting(lines, setting);
         return lines;
      }

      // a settings.txt written by an older version is missing the newer keys; add them to the
      // file with their documentation, AND to the in-memory map so the new setting works this
      // session rather than only after the next restart
      private static void AppendMissingSettings(string settingsFile, Dictionary<string, string> loaded)
      {
         var lines = new List<string>();
         foreach (Setting setting in Documented)
            if (!loaded.ContainsKey(setting.Key))
            {
               AppendSetting(lines, setting);
               loaded[setting.Key] = setting.Default;
            }
         if (lines.Count == 0) return;

         try { File.AppendAllLines(settingsFile, lines); }
         catch { }   // read-only or locked settings file: the append is cosmetic, the value is already live
      }

      private static void AppendSetting(List<string> lines, Setting setting)
      {
         lines.Add("");
         lines.AddRange(setting.Description);
         lines.Add(setting.Key + "=" + setting.Default);
      }
   }
}
