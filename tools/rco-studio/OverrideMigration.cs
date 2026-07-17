using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

namespace RcoStudio
{
   // re-bases a mod's layout "override" fields onto current firmware. Sony shifted these
   // pointers into the XMB layout tables at firmware 4.89 (and only in the RCOs that referenced
   // values past the shift point), which broke every pre-4.89 mod's positioning. we ship the
   // correct 4.93 value for each object of each stock RCO and copy those onto the mod, matched
   // by rco name + object name. only these six fields are touched -- images, positions and
   // colours the modder chose are left exactly as they are.
   public static class OverrideMigration
   {
      // the six fields between onLoad and image: rcomage's "standard position definition" block,
      // which is what the wiki calls the object's override values
      private static readonly string[] OverrideFields =
         { "unknown17", "unknown18", "unknown19", "unknownInt20", "unknownInt21", "unknown22" };

      private static readonly string DataFile = Path.Combine(ToolRunner.BaseDir, "tools\\override-data.txt");

      // rcoName -> (objectName -> the six field values, in OverrideFields order)
      private static Dictionary<string, Dictionary<string, string[]>> data;

      public class Result
      {
         public bool RcoInData;             // false when the stock data has no such rco (a custom rco)
         public int ObjectsUpdated;         // objects whose override values were changed
         public int ObjectsAlreadyCurrent;  // matched, but values already equal to stock
         public List<string> Unmatched = new List<string>();   // objects with no stock counterpart
      }

      public static bool DataAvailable { get { return File.Exists(DataFile); } }

      // applies current-firmware overrides to a mod's structure xml, in place. the file only
      // changes where a value actually differs, so a mod already on current firmware is untouched.
      public static Result Migrate(RcoJob job, Action<string> log)
      {
         Load();
         var result = new Result();
         Dictionary<string, string[]> stock;
         if (!data.TryGetValue(job.Name, out stock)) { result.RcoInData = false; return result; }
         result.RcoInData = true;

         string xmlFile = Path.Combine(job.DumpDir, job.Name + ".xml");
         string[] lines = File.ReadAllLines(xmlFile);
         bool changed = false;

         for (int index = 0; index < lines.Length; index++)
         {
            string name = ReadAttribute(lines[index], "name");
            if (name == "" || lines[index].IndexOf("unknown17=", StringComparison.Ordinal) < 0) continue;   // not an override-bearing object

            string[] stockValues;
            if (!stock.TryGetValue(name, out stockValues)) { result.Unmatched.Add(name); continue; }

            string updated = ApplyOverrides(lines[index], stockValues);
            if (updated != lines[index]) { lines[index] = updated; changed = true; result.ObjectsUpdated++; }
            else result.ObjectsAlreadyCurrent++;
         }

         if (changed) File.WriteAllLines(xmlFile, lines);
         return result;
      }

      // sets each override attribute on an object line to its stock value, leaving all else intact
      private static string ApplyOverrides(string line, string[] stockValues)
      {
         for (int field = 0; field < OverrideFields.Length; field++)
            line = Regex.Replace(line, OverrideFields[field] + "=\"[^\"]*\"",
                                 OverrideFields[field] + "=\"" + stockValues[field] + "\"");
         return line;
      }

      private static string ReadAttribute(string line, string attribute)
      {
         Match match = Regex.Match(line, attribute + "=\"([^\"]*)\"");
         return match.Success ? match.Groups[1].Value : "";
      }

      // parses the bundled data once: "@rcoName" starts a section, then "objName v17 v18 ... v22"
      private static void Load()
      {
         if (data != null) return;
         data = new Dictionary<string, Dictionary<string, string[]>>();
         if (!File.Exists(DataFile)) return;

         Dictionary<string, string[]> current = null;
         foreach (string raw in File.ReadAllLines(DataFile))
         {
            string line = raw.Trim();
            if (line == "" || line.StartsWith("#")) continue;
            if (line[0] == '@') { current = new Dictionary<string, string[]>(); data[line.Substring(1)] = current; continue; }
            if (current == null) continue;

            string[] parts = line.Split(' ');
            if (parts.Length != 7) continue;
            var values = new string[6];
            Array.Copy(parts, 1, values, 0, 6);
            current[parts[0]] = values;
         }
      }
   }
}
