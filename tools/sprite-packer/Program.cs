using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;

namespace SpritePacker
{
   internal static class Program
   {
     // parsed command line. input directory is the only required field.
     private sealed class Options
     {
       public string InputDir;
       public string OutputDir;   // -o, defaults to the input directory
       public string Name;        // -n, defaults to the input directory's name
       public string HeaderDir;   // -h, defaults to the output directory
       public string Prefix;      // -p, drives enum / array / header-file naming
     }

     private static int Main(string[] args)
     {
       try
       {
         Options options;
         string error;
         if (!TryParseArguments(args, out options, out error))
         {
            Console.Error.WriteLine(error);
            PrintUsage();
            return Pause(2);
         }

         if (!Directory.Exists(options.InputDir))
         {
            Console.Error.WriteLine("input directory not found: " + options.InputDir);
            return Pause(2);
         }

         // resolve defaults
         string outputDir = options.OutputDir ?? options.InputDir;
         string name = options.Name ?? new DirectoryInfo(options.InputDir).Name;
         string headerDir = options.HeaderDir ?? outputDir;
         Directory.CreateDirectory(outputDir);
         Directory.CreateDirectory(headerDir);

         string headerFileName = options.Prefix != null ? options.Prefix + "-regions.h" : "sprite-regions.h";
         string sheetPath = Path.Combine(outputDir, name + ".png");
         string headerPath = Path.Combine(headerDir, headerFileName);

         // load -> pack -> write
         List<Sprite> sprites = SpriteLoader.Load(options.InputDir);
         if (sprites.Count == 0)
         {
            Console.Error.WriteLine("no .png files found in: " + options.InputDir);
            return Pause(2);
         }

         Size sheetSize = Packer.Pack(sprites);
         SheetWriter.Write(sprites, sheetSize.Width, sheetSize.Height, sheetPath);
         HeaderWriter.Write(sprites, headerPath, options.Prefix);

         Console.WriteLine("packed {0} sprites into {1}x{2}", sprites.Count, sheetSize.Width, sheetSize.Height);
         Console.WriteLine("  sheet  -> " + sheetPath);
         Console.WriteLine("  header -> " + headerPath);
         return 0;
       }
       catch (Exception error)
       {
         Console.Error.WriteLine("error: " + error.Message);
         return Pause(1);
       }
     }

     // parses "<inputDir> [-o dir] [-n name] [-h dir] [-p prefix]". the first
     // non-flag argument is the input directory; flags may appear in any order.
     private static bool TryParseArguments(string[] args, out Options options, out string error)
     {
       options = new Options();
       error = null;

       for (int i = 0; i < args.Length; i++)
       {
         switch (args[i])
         {
            case "-o": if (!TakeValue(args, ref i, out options.OutputDir, out error)) return false; break;
            case "-n": if (!TakeValue(args, ref i, out options.Name, out error)) return false; break;
            case "-h": if (!TakeValue(args, ref i, out options.HeaderDir, out error)) return false; break;
            case "-p": if (!TakeValue(args, ref i, out options.Prefix, out error)) return false; break;
            default:
              if (options.InputDir != null) { error = "unexpected argument: " + args[i]; return false; }
              options.InputDir = args[i];
              break;
         }
       }

       if (options.InputDir == null) { error = "missing input directory"; return false; }
       return true;
     }

     // consumes the value following the flag at args[index], advancing index past it.
     private static bool TakeValue(string[] args, ref int index, out string value, out string error)
     {
       if (index + 1 >= args.Length)
       {
         value = null;
         error = "missing value for " + args[index];
         return false;
       }
       value = args[++index];
       error = null;
       return true;
     }

     private static void PrintUsage()
     {
       Console.Error.WriteLine("usage: sprite-packer <inputDir> [-o outputDir] [-n name] [-h headerDir] [-p prefix]");
       Console.Error.WriteLine("       (or drag a folder onto the exe)");
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
