using System;
using System.IO;

namespace XmlToSfo
{
    // usage: xml-to-sfo <paramsfo.xml> [outputPath]
    //
    // Without an explicit output path, writes PARAM.SFO next to the input.
    // outputPath may be a full file path or a directory (PARAM.SFO is appended).
    //
    // On any parse / validation error, prints a message and pauses so the
    // window stays open when launched via drag-drop.

    internal static class Program
    {
        private const string DefaultOutputName = "PARAM.SFO";

        private static int Main(string[] args)
        {
            try
            {
                if (args.Length < 1 || args.Length > 2)
                {
                    Console.Error.WriteLine("usage: xml-to-sfo <paramsfo.xml> [outputPath]");
                    Console.Error.WriteLine("       (or drag an .xml file onto the exe)");
                    return Pause(2);
                }

                string input = args[0];
                if (!File.Exists(input))
                {
                    Console.Error.WriteLine("input not found: " + input);
                    return Pause(2);
                }

                SfoModel model = SfoXmlParser.Parse(input);

                string defaultDir = Path.GetDirectoryName(Path.GetFullPath(input));
                string outPath = ResolveOutputPath(args.Length > 1 ? args[1] : null, defaultDir);

                EnsureDirectory(outPath);

                SfoBuilder.Write(model, outPath);

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
        //   - null/empty -> defaultDir/PARAM.SFO
        //   - existing directory or path with trailing separator -> append PARAM.SFO
        //   - otherwise -> treat as full file path
        private static string ResolveOutputPath(string raw, string defaultDir)
        {
            if (string.IsNullOrEmpty(raw))
                return Path.Combine(defaultDir, DefaultOutputName);

            string full = Path.GetFullPath(raw);
            bool looksLikeDir = raw.EndsWith("\\") || raw.EndsWith("/")
                                || (Directory.Exists(full));
            if (looksLikeDir)
                return Path.Combine(full, DefaultOutputName);

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
            try { Console.ReadKey(true); } catch { } // no console available
            return code;
        }
    }
}
