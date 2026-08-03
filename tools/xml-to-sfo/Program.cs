using System;
using System.Collections.Generic;
using System.IO;

namespace XmlToSfo
{
   // usage: xml-to-sfo <paramsfo.xml> [outputPath] [-version <text>]
   //
   // without an explicit output path, writes PARAM.SFO next to the input.
   // outputPath may be a full file path or a directory (PARAM.SFO is appended).
   // -version replaces the xml's APP_VER, so the build can stamp its number in.
   //
   // on any parse / validation error, prints a message and pauses so the window
   // stays open when launched via drag-drop.
   internal static class Program
   {
     private const string DefaultOutputName = "PARAM.SFO";

     private static int Main(string[] args)
     {
       try
       {
         string appVersion = null;
         var paths = new List<string>();
         for (int i = 0; i < args.Length; i++)
         {
            if (args[i] == "-version" && i + 1 < args.Length) appVersion = args[++i];
            else paths.Add(args[i]);
         }

         if (paths.Count < 1 || paths.Count > 2)
         {
            Console.Error.WriteLine("usage: xml-to-sfo <paramsfo.xml> [outputPath] [-version <text>]");
            Console.Error.WriteLine("       (or drag an .xml file onto the exe)");
            return Pause(2);
         }

         string inputPath = paths[0];
         if (!File.Exists(inputPath))
         {
            Console.Error.WriteLine("input not found: " + inputPath);
            return Pause(2);
         }

         SfoModel model = SfoXmlParser.Parse(inputPath, appVersion);

         string inputDirectory = Path.GetDirectoryName(Path.GetFullPath(inputPath));
         string outputPath = ResolveOutputPath(paths.Count > 1 ? paths[1] : null, inputDirectory);
         EnsureDirectory(outputPath);

         SfoBuilder.Write(model, outputPath);
         return 0;   // success: silent, auto-exit
       }
       catch (ParseException parseError)
       {
         Console.Error.WriteLine("error: " + parseError.Message);
         return Pause(1);
       }
       catch (Exception error)
       {
         Console.Error.WriteLine("error: " + error.Message);
         return Pause(1);
       }
     }

     // resolves an optional cli path. three cases:
     //   null/empty                                  -> defaultDirectory/PARAM.SFO
     //   existing directory or trailing separator    -> append PARAM.SFO
     //   otherwise                                    -> treat as a full file path
     private static string ResolveOutputPath(string raw, string defaultDirectory)
     {
       if (string.IsNullOrEmpty(raw))
         return Path.Combine(defaultDirectory, DefaultOutputName);

       string full = Path.GetFullPath(raw);
       bool looksLikeDirectory = raw.EndsWith("\\") || raw.EndsWith("/") || Directory.Exists(full);
       return looksLikeDirectory ? Path.Combine(full, DefaultOutputName) : full;
     }

     private static void EnsureDirectory(string filePath)
     {
       string directory = Path.GetDirectoryName(filePath);
       if (!string.IsNullOrEmpty(directory) && !Directory.Exists(directory))
         Directory.CreateDirectory(directory);
     }

     private static int Pause(int exitCode)
     {
       Console.Error.WriteLine();
       Console.Error.WriteLine("Press any key to close...");
       try { Console.ReadKey(true); } catch { }   // no console available
       return exitCode;
     }
   }
}
