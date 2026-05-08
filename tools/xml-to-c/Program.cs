using System;
using System.IO;

namespace XmlToC
{
    // usage: xml-to-c <screen.xml> [cPath] [hPath]
    //
    // Without optional paths, writes <name>.c and <name>.h next to the input
    // and exits silently. Each optional path may be a full file path or a
    // directory (the screen name + extension are appended in the latter case).
    //
    // On any parse / validation error, prints a message and pauses so the
    // window stays open when launched via drag-drop.

    internal static class Program
    {
        private static int Main(string[] args)
        {
            try
            {
                if (args.Length < 1 || args.Length > 3)
                {
                    Console.Error.WriteLine("usage: xml-to-c <screen.xml> [cPath] [hPath]");
                    Console.Error.WriteLine("       (or drag an .xml file onto the exe)");
                    return Pause(2);
                }

                string input = args[0];
                if (!File.Exists(input))
                {
                    Console.Error.WriteLine("input not found: " + input);
                    return Pause(2);
                }

                ScreenModel model = XmlParser.Parse(input);

                string inputBaseName = Path.GetFileNameWithoutExtension(input);
                string defaultDir = Path.GetDirectoryName(Path.GetFullPath(input));
                string cPath = ResolveOutputPath(args.Length > 1 ? args[1] : null, defaultDir, inputBaseName, ".c");
                string hPath = ResolveOutputPath(args.Length > 2 ? args[2] : null, defaultDir, inputBaseName, ".h");

                EnsureDirectory(cPath);
                EnsureDirectory(hPath);

                CodeGenerator.Emit(model, cPath, hPath);

                // success: silent, auto-exit
                return 0;
            }
            catch (ParseException pex)
            {
                Console.Error.WriteLine("error: " + pex.Message);
                return Pause(1);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("error: " + ex.Message);
                return Pause(1);
            }
        }

        // Resolves an optional CLI path. Three cases:
        //   - null/empty -> default into defaultDir as <name><ext>
        //   - existing directory or path with trailing separator -> append <name><ext>
        //   - otherwise -> treat as full file path (caller's responsibility to suffix correctly)
        private static string ResolveOutputPath(string raw, string defaultDir, string name, string ext)
        {
            if (string.IsNullOrEmpty(raw))
                return Path.Combine(defaultDir, name + ext);

            string full = Path.GetFullPath(raw);
            bool looksLikeDir = raw.EndsWith("\\") || raw.EndsWith("/")
                                || (Directory.Exists(full));
            if (looksLikeDir)
                return Path.Combine(full, name + ext);

            return full;
        }

        private static void EnsureDirectory(string filePath)
        {
            string dir = Path.GetDirectoryName(filePath);
            if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                Directory.CreateDirectory(dir);
        }

        private static int Pause(int code)
        {
            Console.Error.WriteLine();
            Console.Error.WriteLine("Press any key to close...");
            try { Console.ReadKey(true); } catch { /* no console available */ }
            return code;
        }
    }
}
