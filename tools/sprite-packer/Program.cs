using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;

namespace SpritePacker
{
    internal static class Program
    {
        private static int Main(string[] args)
        {
            try
            {
                string inputDir = null;
                string outputDir = null;
                string name = null;
                string headerPath = null;
                string prefix = null;

                int i = 0;
                while (i < args.Length)
                {
                    if (args[i] == "-o" && i + 1 < args.Length) { outputDir = args[++i]; }
                    else if (args[i] == "-n" && i + 1 < args.Length) { name = args[++i]; }
                    else if (args[i] == "-h" && i + 1 < args.Length) { headerPath = args[++i]; }
                    else if (args[i] == "-p" && i + 1 < args.Length) { prefix = args[++i]; }
                    else if (inputDir == null) { inputDir = args[i]; }
                    else
                    {
                        Console.Error.WriteLine("unexpected argument: " + args[i]);
                        return Pause(2);
                    }
                    i++;
                }

                if (inputDir == null)
                {
                    Console.Error.WriteLine("usage: sprite-packer <inputDir> [-o outputDir] [-n name] [-h headerDir]");
                    Console.Error.WriteLine("       (or drag a folder onto the exe)");
                    return Pause(2);
                }

                if (!Directory.Exists(inputDir))
                {
                    Console.Error.WriteLine("input directory not found: " + inputDir);
                    return Pause(2);
                }

                if (outputDir == null) outputDir = inputDir;
                if (name == null) name = new DirectoryInfo(inputDir).Name;
                if (!Directory.Exists(outputDir)) Directory.CreateDirectory(outputDir);

                string headerDir = headerPath ?? outputDir;
                if (!Directory.Exists(headerDir)) Directory.CreateDirectory(headerDir);

                string headerFileName = prefix != null ? prefix + "-regions.h" : "sprite-regions.h";
                string sheetPath = Path.Combine(outputDir, name + ".png");
                headerPath = Path.Combine(headerDir, headerFileName);

                List<Sprite> sprites = SpriteLoader.Load(inputDir);
                if (sprites.Count == 0)
                {
                    Console.Error.WriteLine("no .png files found in: " + inputDir);
                    return Pause(2);
                }

                Size sheetSize = Packer.Pack(sprites);

                SheetWriter.Write(sprites, sheetSize.Width, sheetSize.Height, sheetPath);
                HeaderWriter.Write(sprites, headerPath, prefix);

                Console.WriteLine("packed {0} sprites into {1}x{2}", sprites.Count, sheetSize.Width, sheetSize.Height);
                Console.WriteLine("  sheet  -> " + sheetPath);
                Console.WriteLine("  header -> " + headerPath);
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("error: " + ex.Message);
                return Pause(1);
            }
        }

        private static int Pause(int code)
        {
            Console.Error.WriteLine();
            Console.Error.WriteLine("Press any key to close...");
            try { Console.ReadKey(true); } catch { }
            return code;
        }
    }
}
