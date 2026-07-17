using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace RcoStudio
{
   // wraps the bundled rcomage.exe and GimConv.exe command-line tools.
   // dump: rcomage dumps everything raw (xml references .gim/.vag), then we make
   //       editable .png/.wav siblings ourselves via GimConv / rcomage vagdec, and
   //       keep a pristine copy of the whole dump (see PristineDirName).
   // compile: files the user edited are converted back onto their raw .gim/.vag
   //       first, then rcomage compiles with its own converters disabled -- its
   //       built-in png->gim path crashes, so it must never run.
   public static class ToolRunner
   {
      public static readonly string BaseDir = AppDomain.CurrentDomain.BaseDirectory;
      public static readonly string RcomageDir = Path.Combine(BaseDir, "tools\\Rcomage");
      public static readonly string RcomageExe = Path.Combine(RcomageDir, "rcomage.exe");
      public static readonly string GimConvDir = Path.Combine(BaseDir, "tools\\GimConv");
      public static readonly string GimConvExe = Path.Combine(GimConvDir, "GimConv.exe");
      public static readonly string DumpsDir = Path.Combine(BaseDir, "dumps");
      public static readonly string CompiledDir = Path.Combine(BaseDir, "compiled");

      private const string DumpInfoFileName = "dump-info.txt";

      // a copy of every file exactly as the dump produced it. it is what "edited" is
      // measured against, what Revert restores from, and where compile puts the raw
      // .gim/.vag back from after re-encoding edited images and sounds onto them.
      // the leading dot keeps it out of the rco listing and the preview.
      private const string PristineDirName = ".original";

      // a dump replaces the whole pristine folder while the preview may be reading it. windows
      // will not delete a file someone has open, so without taking turns the swap throws and
      // fails the dump. holds only for the swap itself and for single file reads.
      private static readonly object pristineLock = new object();

      // dumps an rco to editable files. a dump always starts fresh: the caller (AddRcos) only
      // reaches here for a new rco or a deliberate re-dump from a different source, and warns
      // first if that would discard edits -- so nothing here has to preserve user work.
      public static void Dump(RcoJob job, Action<string> log)
      {
         // dump structure + raw resources
         Directory.CreateDirectory(job.DumpDir);
         job.Detail = "dumping structure";
         string arguments = "dump " + Quote(job.RcoPath) + " " + Quote(job.Name + "\\" + job.Name + ".xml") +
                            " --resdir " + Quote(job.Name) + " --ini-dir " + Quote(RcomageDir);
         int exitCode;
         string output = Run(RcomageExe, arguments, DumpsDir, out exitCode);
         if (exitCode != 0) throw new Exception("rcomage dump failed:\n" + LastLines(output, 6));

         // remember header compression (so the recompiled rco matches the original)
         // and the source path (so sets and re-dumps survive an app restart)
         job.HeaderCompressed = ReadHeaderCompression(output);
         File.WriteAllLines(Path.Combine(job.DumpDir, DumpInfoFileName), new[]
         {
            "headerCompression=" + (job.HeaderCompressed ? "zlib" : "none"),
            "sourcePath=" + job.RcoPath
         });

         // make editable png siblings for the gim images (parallel; one GimConv run per file is slow)
         string[] gimFiles = Directory.GetFiles(job.DumpDir, "*.gim");
         int imagesDone = 0;
         Parallel.ForEach(gimFiles, gimFile =>
         {
            string pngFile = Path.ChangeExtension(gimFile, ".png");
            int convertExit;
            Run(GimConvExe, Quote(gimFile) + " -o " + Quote(pngFile), GimConvDir, out convertExit);
            if (convertExit != 0) log("[warn] " + job.Name + ": could not convert " + Path.GetFileName(gimFile) + " to png");
            job.Detail = "converting images " + Interlocked.Increment(ref imagesDone) + "/" + gimFiles.Length;
         });

         // make editable wav siblings for the vag sounds (one wav per multi-channel group)
         var soundGroups = GroupVagChannels(job.DumpDir);
         int soundsDone = 0;
         Parallel.ForEach(soundGroups, sound =>
         {
            string wavFile = Path.Combine(job.DumpDir, sound.Key + ".wav");
            int convertExit;
            Run(RcomageExe, "vagdec " + QuoteAll(sound.Value) + " " + Quote(wavFile), DumpsDir, out convertExit);
            if (convertExit != 0) log("[warn] " + job.Name + ": could not convert " + sound.Key + " to wav");
            job.Detail = "converting sounds " + Interlocked.Increment(ref soundsDone) + "/" + soundGroups.Count;
         });

         // pristine copy = the dump exactly as produced. written last, so a dump that throws
         // partway leaves no pristine folder and FindEditedFiles reports nothing edited.
         WritePristineCopy(job.DumpDir);

         job.Detail = gimFiles.Length + " images, " + soundGroups.Count + " sounds";
      }

      public class CompileResult
      {
         public string OutputRco;
         public string VerifyProblem;   // null when the compiled rco round-tripped exactly
      }

      public static CompileResult Compile(RcoJob job, Action<string> log)
      {
         if (!File.Exists(Path.Combine(job.DumpDir, job.Name + ".xml")))
            throw new Exception("no dump found to compile from");

         // compiling re-encodes edited png/wav onto their raw .gim/.vag, so the dump's raw
         // files end up holding edited data. verify has to run while they still do -- it
         // compares the compiled rco against what was fed to rcomage. only then put the raws
         // back from the pristine copy, so a dump's raws always match the rco it came from.
         try
         {
            ConvertEditedImages(job, log);
            ConvertEditedSounds(job, log);

            // compile with rcomage's own converters off; resources are already in native formats
            Directory.CreateDirectory(CompiledDir);
            var result = new CompileResult { OutputRco = Path.Combine(CompiledDir, job.Name + ".rco") };
            string packHeader = job.HeaderCompressed ? "zlib" : "none";
            string arguments = "compile " + Quote(job.Name + "\\" + job.Name + ".xml") + " " + Quote(result.OutputRco) +
                               " --no-convgim --no-convvag --pack-hdr " + packHeader + " --ini-dir " + Quote(RcomageDir);
            int exitCode;
            string output = Run(RcomageExe, arguments, DumpsDir, out exitCode);
            if (exitCode != 0) throw new Exception("rcomage compile failed:\n" + LastLines(output, 6));

            job.Detail = "verifying";
            result.VerifyProblem = VerifyCompiled(job, result.OutputRco);
            return result;
         }
         finally
         {
            RestoreRawResources(job.DumpDir);
         }
      }

      // section: adding resources the rco never had

      // brings a new png into a dump: converts it to a gim in the chosen format and appends
      // the <Image> entry, without which the rco has no way to reach it. returns the name the
      // image was given, which is what the xml's objects refer to it by.
      public static string AddImage(RcoJob job, string sourcePng, GimFormat format, Action<string> log)
      {
         string xmlFile = Path.Combine(job.DumpDir, job.Name + ".xml");
         RequireTree(xmlFile, "ImageTree", job.Name, "images");   // before writing anything

         string imageName = MakeUniqueName(xmlFile, "Image", Path.GetFileNameWithoutExtension(sourcePng));
         string pngFile = Path.Combine(job.DumpDir, imageName + ".png");
         string gimFile = Path.Combine(job.DumpDir, imageName + ".gim");
         File.Copy(sourcePng, pngFile, true);

         int exitCode;
         string flags = GetGimConvFlags((int)format, 0, true);   // ps3: big-endian, normal pixel order
         string output = Run(GimConvExe, Quote(pngFile) + " -o " + Quote(gimFile) + flags, GimConvDir, out exitCode);
         if (exitCode != 0 || !File.Exists(gimFile))
         {
            // leave nothing half-added: GimConv may have written the gim before failing, and an
            // orphan gim with no xml entry can't be reached or removed through the ui
            File.Delete(pngFile);
            if (File.Exists(gimFile)) File.Delete(gimFile);
            throw new Exception("GimConv could not convert " + Path.GetFileName(sourcePng) + ":\n" + LastLines(output, 4));
         }

         InsertTreeEntry(xmlFile, "ImageTree",
            "<Image name=\"" + imageName + "\" src=\"" + job.Name + "\\" + imageName + ".gim\"" +
            " format=\"gim\" compression=\"zlib\" unknownByte=\"0\" />");
         log("  added image " + imageName + " as " + format + " (" + flags.Trim() + ")");
         return imageName;
      }

      // brings a new wav into a dump: encodes it to one vag per channel and appends the
      // <Sound> entry. returns the name the sound was given.
      public static string AddSound(RcoJob job, string sourceWav, Action<string> log)
      {
         string xmlFile = Path.Combine(job.DumpDir, job.Name + ".xml");
         RequireTree(xmlFile, "SoundTree", job.Name, "sounds");

         // vagenc wants 16-bit pcm; check it here so the message names the real problem rather
         // than surfacing a raw tool error two steps later
         WavFormat wav = ReadWavFormat(sourceWav);
         if ((wav.Channels != 1 && wav.Channels != 2) || wav.BitsPerSample != 16)
            throw new Exception(Path.GetFileName(sourceWav) + " must be a mono or stereo 16-bit PCM wav");

         string soundName = MakeUniqueName(xmlFile, "Sound", Path.GetFileNameWithoutExtension(sourceWav));
         string wavFile = Path.Combine(job.DumpDir, soundName + ".wav");
         File.Copy(sourceWav, wavFile, true);

         var channelFiles = new List<string>();
         for (int channel = 0; channel < wav.Channels; channel++)
            channelFiles.Add(Path.Combine(job.DumpDir, soundName + ".ch" + channel + ".vag"));

         try { EncodeWavToVag(wavFile, channelFiles, log); }
         catch
         {
            File.Delete(wavFile);   // leave no half-added sound behind
            foreach (string channelFile in channelFiles) if (File.Exists(channelFile)) File.Delete(channelFile);
            throw;
         }

         // src uses a ch* wildcard whatever the channel count -- mono sounds are just a lone ch0
         InsertTreeEntry(xmlFile, "SoundTree",
            "<Sound name=\"" + soundName + "\" src=\"" + job.Name + "\\" + soundName + ".ch*.vag\"" +
            " format=\"vag\" channels=\"" + wav.Channels + "\" />");
         log("  added sound " + soundName + " (" + wav.Channels + " channel(s))");
         return soundName;
      }

      // an rco only has the resource trees its plugin was built to use, and rcomage cannot
      // invent one -- an rco with no sounds has no way to reach a sound you add to it
      public static bool HasTree(RcoJob job, string treeName)
      {
         string xmlFile = Path.Combine(job.DumpDir, job.Name + ".xml");
         return File.Exists(xmlFile) && File.ReadAllText(xmlFile).IndexOf("</" + treeName + ">", StringComparison.Ordinal) >= 0;
      }

      private static void RequireTree(string xmlFile, string treeName, string rcoName, string what)
      {
         if (File.ReadAllText(xmlFile).IndexOf("</" + treeName + ">", StringComparison.Ordinal) < 0)
            throw new Exception(rcoName + " has no " + what + " at all, so there is nothing for a new one to attach to");
      }

      // adds an entry as the last child of a resource tree, as text so the rest of the xml stays
      // byte-identical. it goes in at the START of the closing tag's line, reusing that line's
      // indentation one level deeper -- inserting at the tag itself lands after the line's
      // leading tabs and strands the tag.
      private static void InsertTreeEntry(string xmlFile, string treeName, string entryElement)
      {
         string xml = File.ReadAllText(xmlFile);
         int closingTag = xml.LastIndexOf("</" + treeName + ">", StringComparison.Ordinal);
         int lineStart = xml.LastIndexOf('\n', closingTag - 1) + 1;
         string indent = xml.Substring(lineStart, closingTag - lineStart);
         File.WriteAllText(xmlFile, xml.Substring(0, lineStart) + indent + "\t" + entryElement + "\r\n" + xml.Substring(lineStart));
      }

      // vag stores audio in blocks of 28 samples, and rcomage's vagenc rejects a wav that is
      // not a whole number of them ("premature end") rather than padding it itself
      private const int VagSamplesPerBlock = 28;

      private struct WavFormat
      {
         public int Channels;
         public int BitsPerSample;
         public int DataOffset;     // first byte of sample data
         public int DataLength;     // bytes of sample data
      }

      // reads a wav's fmt/data chunks. Channels is 0 when this is not a wav we understand.
      private static WavFormat ReadWavFormat(string wavFile)
      {
         var wav = new WavFormat();
         byte[] data = File.ReadAllBytes(wavFile);
         if (data.Length < 12 || Encoding.ASCII.GetString(data, 0, 4) != "RIFF" || Encoding.ASCII.GetString(data, 8, 4) != "WAVE") return wav;

         int offset = 12;
         while (offset + 8 <= data.Length)
         {
            string chunkId = Encoding.ASCII.GetString(data, offset, 4);
            int chunkSize = BitConverter.ToInt32(data, offset + 4);
            if (chunkSize < 0) return wav;
            int body = offset + 8;

            if (chunkId == "fmt " && body + 16 <= data.Length)
            {
               wav.Channels = BitConverter.ToUInt16(data, body + 2);
               wav.BitsPerSample = BitConverter.ToUInt16(data, body + 14);
            }
            else if (chunkId == "data")
            {
               wav.DataOffset = body;
               wav.DataLength = Math.Min(chunkSize, data.Length - body);
               return wav;
            }
            offset = body + chunkSize + (chunkSize & 1);   // chunks are word-aligned
         }
         return wav;
      }

      // encodes a wav to one vag per channel, padding a copy with silence when its length is
      // not a whole number of vag blocks. the user's own wav is never modified -- the padding
      // is at most 27 samples (well under a millisecond) and only exists for the encoder.
      private static void EncodeWavToVag(string wavFile, List<string> channelFiles, Action<string> log)
      {
         string encodeFrom = wavFile;
         string paddedFile = "";
         WavFormat wav = ReadWavFormat(wavFile);
         int bytesPerSample = wav.Channels * (wav.BitsPerSample / 8);

         if (bytesPerSample > 0)
         {
            int samples = wav.DataLength / bytesPerSample;
            int remainder = samples % VagSamplesPerBlock;
            if (remainder != 0)
            {
               paddedFile = wavFile + ".padded.wav";
               WritePaddedWav(wavFile, wav, (VagSamplesPerBlock - remainder) * bytesPerSample, paddedFile);
               encodeFrom = paddedFile;
               log("  padded " + Path.GetFileName(wavFile) + " with " + (VagSamplesPerBlock - remainder) + " silent samples (vag needs whole 28-sample blocks)");
            }
         }

         try
         {
            int exitCode;
            string output = Run(RcomageExe, "vagenc " + Quote(encodeFrom) + " " + QuoteAll(channelFiles), DumpsDir, out exitCode);
            if (exitCode != 0) throw new Exception("could not encode " + Path.GetFileName(wavFile) + " to vag:\n" + LastLines(output, 4));
         }
         finally
         {
            if (paddedFile != "" && File.Exists(paddedFile)) File.Delete(paddedFile);
         }
      }

      // the same wav with extra silence on the end, keeping only the chunks vagenc reads
      private static void WritePaddedWav(string wavFile, WavFormat wav, int extraBytes, string paddedFile)
      {
         byte[] data = File.ReadAllBytes(wavFile);
         using (var writer = new BinaryWriter(File.Create(paddedFile)))
         {
            writer.Write(data, 0, wav.DataOffset);                     // riff header, fmt, up to the sample data
            writer.Write(data, wav.DataOffset, wav.DataLength);
            writer.Write(new byte[extraBytes]);                        // 16-bit pcm silence is zeroes
            writer.Seek(4, SeekOrigin.Begin);
            writer.Write(wav.DataOffset + wav.DataLength + extraBytes - 8);   // riff size
            writer.Seek(wav.DataOffset - 4, SeekOrigin.Begin);
            writer.Write(wav.DataLength + extraBytes);                        // data chunk size
         }
      }

      // what formats this rco's images already use, e.g. "Rgba8888 x12, Dxt5 x3".
      // shown when picking a format for a new image, since matching its neighbours is usually right.
      public static string SummariseImageFormats(string dumpDir)
      {
         var counts = new SortedDictionary<string, int>();
         foreach (string gimFile in Directory.GetFiles(dumpDir, "*.gim"))
         {
            int format, pixelOrder; bool bigEndian;
            if (!ReadGimFormat(gimFile, out format, out pixelOrder, out bigEndian)) continue;
            string name = Enum.IsDefined(typeof(GimFormat), format) ? ((GimFormat)format).ToString() : "format " + format;
            counts[name] = counts.ContainsKey(name) ? counts[name] + 1 : 1;
         }
         var parts = new List<string>();
         foreach (KeyValuePair<string, int> entry in counts) parts.Add(entry.Key + " x" + entry.Value);
         return parts.Count == 0 ? "this RCO has no images yet" : string.Join(", ", parts.ToArray());
      }

      // a resource name is an xml attribute other objects refer to, so keep it plain and unique.
      // elementName is the tag it must not clash with, e.g. "Image" or "Sound".
      private static string MakeUniqueName(string xmlFile, string elementName, string fromFileName)
      {
         var safe = new StringBuilder();
         foreach (char character in fromFileName)
            safe.Append(char.IsLetterOrDigit(character) || character == '_' ? character : '_');
         string wanted = safe.ToString().Trim('_');
         if (wanted == "") wanted = elementName.ToLowerInvariant();

         string xml = File.ReadAllText(xmlFile);
         string name = wanted;
         for (int suffix = 2; xml.Contains("<" + elementName + " name=\"" + name + "\""); suffix++) name = wanted + "_" + suffix;
         return name;
      }

      private static string GetPristineDir(string dumpDir) { return Path.Combine(dumpDir, PristineDirName); }

      // a dump made before the folder was renamed keeps its edit-tracking and revert without a
      // re-dump: move the old .pristine into place if the new .original isn't there yet
      public static void MigrateOriginalFolder(string dumpDir)
      {
         string old = Path.Combine(dumpDir, ".pristine");
         string current = GetPristineDir(dumpDir);
         try { if (Directory.Exists(old) && !Directory.Exists(current)) Directory.Move(old, current); }
         catch { }
      }

      // the dump as rcomage and our converters produced it, before any edit.
      // built alongside and swapped in at the end: filling the real folder in place would leave
      // it half populated for seconds, and anything reading it then (the preview) would take
      // every not-yet-copied file for one the user had added.
      private static void WritePristineCopy(string dumpDir)
      {
         string pristineDir = GetPristineDir(dumpDir);
         string buildingDir = pristineDir + ".building";
         if (Directory.Exists(buildingDir)) Directory.Delete(buildingDir, true);
         Directory.CreateDirectory(buildingDir);
         foreach (string file in Directory.GetFiles(dumpDir))
            File.Copy(file, Path.Combine(buildingDir, Path.GetFileName(file)));

         lock (pristineLock)
         {
            if (Directory.Exists(pristineDir)) Directory.Delete(pristineDir, true);
            Directory.Move(buildingDir, pristineDir);
         }
      }

      // the pristine copy of a dumped file ("" when there is none -- e.g. a file the user added)
      public static string GetPristineCopy(string file)
      {
         string pristineFile = Path.Combine(GetPristineDir(Path.GetDirectoryName(file)), Path.GetFileName(file));
         return File.Exists(pristineFile) ? pristineFile : "";
      }

      // edited = the file differs from its pristine copy. a file with no pristine copy is
      // one the user added, which counts as edited too.
      public static bool IsEdited(string file)
      {
         lock (pristineLock)
         {
            string pristineFile = GetPristineCopy(file);
            if (pristineFile == "") return true;

            // content decides, but comparing every byte of every dump is far too slow to do per
            // row. File.Copy carried the write time across, so an untouched file still matches
            // its copy exactly -- saving something always moves it. only look inside when the
            // cheap facts disagree, so the answer is still content, never the clock.
            var live = new FileInfo(file);
            var dumped = new FileInfo(pristineFile);
            if (live.Length != dumped.Length) return true;
            if (live.LastWriteTimeUtc == dumped.LastWriteTimeUtc) return false;
            return !FilesEqual(file, pristineFile);
         }
      }

      // undoes compile's re-encoding: raw .gim/.vag back to what the dump produced
      private static void RestoreRawResources(string dumpDir)
      {
         lock (pristineLock)
         {
            foreach (string file in Directory.GetFiles(dumpDir))
            {
               if (!IsRawResource(file)) continue;
               string pristineFile = GetPristineCopy(file);
               try { if (pristineFile != "" && !FilesEqual(file, pristineFile)) File.Copy(pristineFile, file, true); }
               catch { }
            }
         }
      }

      // everything the user changed in a dump: any editable file -- png/wav siblings, the
      // structure xml, embedded txt/xml resources -- whose content differs from the pristine copy
      public static List<string> FindEditedFiles(string dumpDir)
      {
         var edited = new List<string>();
         if (!Directory.Exists(GetPristineDir(dumpDir))) return edited;   // dumped before pristine copies existed
         foreach (string file in Directory.GetFiles(dumpDir))
         {
            if (Path.GetFileName(file) == DumpInfoFileName) continue;
            // a dumped raw .gim/.vag is rebuilt from its editable sibling, so it is never an
            // edit in itself. one the user added is different: it is the only record of the
            // format they chose, so it has to travel with the png in a patch.
            if (IsRawResource(file) && GetPristineCopy(file) != "") continue;
            if (IsEdited(file)) edited.Add(file);
         }
         edited.Sort();
         return edited;
      }

      // .gim/.vag are the console-format copies; their editable faces are the png/wav siblings
      public static bool IsRawResource(string file)
      {
         string extension = Path.GetExtension(file).ToLowerInvariant();
         return extension == ".gim" || extension == ".vag";
      }

      // re-dumps a freshly compiled rco and compares it against the working dump.
      // returns null when everything matches, else a short description of what differs.
      // must run before RestoreRawResources -- see Compile.
      private static string VerifyCompiled(RcoJob job, string compiledRco)
      {
         string verifyRoot = Path.Combine(DumpsDir, ".verify");
         string verifyDir = Path.Combine(verifyRoot, job.Name);
         if (Directory.Exists(verifyDir)) Directory.Delete(verifyDir, true);
         Directory.CreateDirectory(verifyDir);
         try
         {
            // re-dump with the same relative resdir so the xml's resource paths match
            string arguments = "dump " + Quote(compiledRco) + " " + Quote(job.Name + "\\" + job.Name + ".xml") +
                               " --resdir " + Quote(job.Name) + " --ini-dir " + Quote(RcomageDir);
            int exitCode;
            string output = Run(RcomageExe, arguments, verifyRoot, out exitCode);
            if (exitCode != 0) return "verify re-dump failed: " + LastLines(output, 3);

            var differing = new List<string>();
            foreach (string verifyFile in Directory.GetFiles(verifyDir))
            {
               string originalFile = Path.Combine(job.DumpDir, Path.GetFileName(verifyFile));
               if (!FilesEqual(verifyFile, originalFile)) differing.Add(Path.GetFileName(verifyFile));
            }
            if (differing.Count == 0) return null;
            return differing.Count + " file(s) differ: " + string.Join(", ", differing.ToArray());
         }
         finally
         {
            try { Directory.Delete(verifyDir, true); } catch { }
         }
      }

      private static bool FilesEqual(string pathA, string pathB)
      {
         if (!File.Exists(pathA) || !File.Exists(pathB)) return false;
         byte[] bytesA = File.ReadAllBytes(pathA);
         byte[] bytesB = File.ReadAllBytes(pathB);
         if (bytesA.Length != bytesB.Length) return false;
         for (int index = 0; index < bytesA.Length; index++)
            if (bytesA[index] != bytesB[index]) return false;
         return true;
      }

      // reads headerCompression from an earlier session's dump-info.txt (defaults to compressed)
      public static bool ReadSavedHeaderCompression(string dumpDir)
      {
         string infoFile = Path.Combine(dumpDir, DumpInfoFileName);
         if (!File.Exists(infoFile)) return true;
         return !File.ReadAllText(infoFile).Contains("headerCompression=none");
      }

      // reads the source .rco path from an earlier session's dump-info.txt ("" if unknown)
      public static string ReadSavedSourcePath(string dumpDir)
      {
         string infoFile = Path.Combine(dumpDir, DumpInfoFileName);
         if (!File.Exists(infoFile)) return "";
         foreach (string line in File.ReadAllLines(infoFile))
            if (line.StartsWith("sourcePath=")) return line.Substring("sourcePath=".Length).Trim();
         return "";
      }

      // an edited png -> convert back onto its gim, in that gim's original format
      private static void ConvertEditedImages(RcoJob job, Action<string> log)
      {
         foreach (string gimFile in Directory.GetFiles(job.DumpDir, "*.gim"))
         {
            string pngFile = Path.ChangeExtension(gimFile, ".png");
            if (!File.Exists(pngFile) || !IsEdited(pngFile)) continue;

            int exitCode;
            string formatFlags = ReadGimConvFlags(gimFile, log);
            string output = Run(GimConvExe, Quote(pngFile) + " -o " + Quote(gimFile) + formatFlags, GimConvDir, out exitCode);
            if (exitCode != 0) throw new Exception("GimConv failed on edited image " + Path.GetFileName(pngFile) + ":\n" + LastLines(output, 4));
            log("  converted edited " + Path.GetFileName(pngFile) + " back to gim (" + formatFlags.Trim() + ")");
         }
      }

      // gim pixel formats, from the header's image block
      public enum GimFormat { Rgba5650 = 0, Rgba5551 = 1, Rgba4444 = 2, Rgba8888 = 3, Index4 = 4, Index8 = 5, Dxt1 = 8, Dxt3 = 9, Dxt5 = 10 }

      // reads a gim's pixel format and storage order from its header image block.
      // returns false when the header is unrecognised or holds no image block (format stays -1).
      // the file is untrusted (a patch can carry any bytes), so the block walk is bounds-checked.
      private static bool ReadGimFormat(string gimFile, out int format, out int pixelOrder, out bool bigEndian)
      {
         format = -1; pixelOrder = -1; bigEndian = true;
         byte[] data = File.ReadAllBytes(gimFile);
         if (data.Length > 0x60 && data[0] == '.' && data[1] == 'G') bigEndian = true;
         else if (data.Length > 0x60 && data[0] == 'M' && data[1] == 'I') bigEndian = false;
         else return false;

         // walk blocks: 2/3 are containers (descend), 4 is the image block we want
         int offset = 0x10;
         while (offset + 0x18 <= data.Length)
         {
            int blockId = ReadU16(data, offset, bigEndian);
            if (blockId == 2 || blockId == 3) { offset += 0x10; continue; }
            if (blockId == 4)
            {
               format = ReadU16(data, offset + 0x14, bigEndian);
               pixelOrder = ReadU16(data, offset + 0x16, bigEndian);
               return true;
            }
            // a block size that doesn't advance offset inside the buffer would loop forever or
            // overflow offset negative and index out of bounds -- reject the file instead
            int blockSize = ReadU32(data, offset + 4, bigEndian);
            if (blockSize <= 0 || blockSize > data.Length - offset) break;
            offset += blockSize;
         }
         return false;   // no image block found
      }

      // true when editing this resource re-encodes into a lossy console format:
      // DXT-compressed images or any sound (VAG is a lossy ADPCM codec)
      public static bool IsLossyFormat(string editableFile)
      {
         string extension = Path.GetExtension(editableFile).ToLowerInvariant();
         if (extension == ".wav") return true;
         if (extension != ".png") return false;
         string gimFile = Path.ChangeExtension(editableFile, ".gim");
         if (!File.Exists(gimFile)) return false;
         int format, pixelOrder; bool bigEndian;
         if (!ReadGimFormat(gimFile, out format, out pixelOrder, out bigEndian)) return false;
         return format == (int)GimFormat.Dxt1 || format == (int)GimFormat.Dxt3 || format == (int)GimFormat.Dxt5;
      }

      // undoes an edit by restoring the pristine copy taken at dump time. that is the dumped
      // state -- the true firmware original only if the rco was dumped from genuine firmware;
      // the tool cannot verify provenance.
      //
      // a resource and its xml entry are one change in two files, so reverting either must undo
      // both: reverting an added png/wav deletes it and drops its entry, and reverting the xml
      // deletes every resource added since the dump, which the restored xml no longer lists.
      public static void RevertResource(string editableFile, RcoJob job, Action<string> log)
      {
         if (GetPristineCopy(editableFile) == "") { RemoveAddedResource(editableFile, job, log); return; }

         lock (pristineLock) File.Copy(GetPristineCopy(editableFile), editableFile, true);
         log("  reverted " + Path.GetFileName(editableFile) + " to dumped state");

         if (editableFile.Equals(Path.Combine(job.DumpDir, job.Name + ".xml"), StringComparison.OrdinalIgnoreCase))
            foreach (string added in FindAddedResources(job)) RemoveAddedResource(added, job, log);
      }

      // the png/wav files in a dump that the user added -- no pristine copy means the dump
      // never produced them
      public static List<string> FindAddedResources(RcoJob job)
      {
         var added = new List<string>();
         if (!Directory.Exists(GetPristineDir(job.DumpDir))) return added;
         foreach (string file in Directory.GetFiles(job.DumpDir))
         {
            string extension = Path.GetExtension(file).ToLowerInvariant();
            if ((extension == ".png" || extension == ".wav") && GetPristineCopy(file) == "") added.Add(file);
         }
         return added;
      }

      // deletes an added resource: the editable file, whatever it was converted to, and its entry
      private static void RemoveAddedResource(string editableFile, RcoJob job, Action<string> log)
      {
         string extension = Path.GetExtension(editableFile).ToLowerInvariant();
         if (extension != ".png" && extension != ".wav")
            throw new Exception("only an image or sound you added can be removed this way");

         string name = Path.GetFileNameWithoutExtension(editableFile);
         var files = new List<string> { editableFile };
         if (extension == ".png") files.Add(Path.ChangeExtension(editableFile, ".gim"));
         else files.AddRange(Directory.GetFiles(job.DumpDir, name + ".ch*.vag"));

         foreach (string file in files) if (File.Exists(file)) File.Delete(file);
         RemoveTreeEntry(Path.Combine(job.DumpDir, job.Name + ".xml"), extension == ".png" ? "Image" : "Sound", name);
         log("  removed added " + (extension == ".png" ? "image " : "sound ") + name + " and its entry");
      }

      // drops one <Image>/<Sound> line, leaving the rest of the xml byte-identical
      private static void RemoveTreeEntry(string xmlFile, string elementName, string name)
      {
         string xml = File.ReadAllText(xmlFile);
         int entry = xml.IndexOf("<" + elementName + " name=\"" + name + "\"", StringComparison.Ordinal);
         if (entry < 0) return;

         int lineStart = xml.LastIndexOf('\n', entry) + 1;      // take the whole line, indent included
         int lineEnd = xml.IndexOf('\n', entry);
         if (lineEnd < 0) lineEnd = xml.Length; else lineEnd++;
         File.WriteAllText(xmlFile, xml.Substring(0, lineStart) + xml.Substring(lineEnd));
      }

      // reads a gim header and returns the GimConv flags that reproduce its exact format:
      // byte order (-rcops3 = ps3 big-endian, added to our GimConv.cfg), pixel format
      // (-bppX) and storage order (-N = normal; GimConv's default is psp 'faster' order)
      private static string ReadGimConvFlags(string gimFile, Action<string> log)
      {
         int format, pixelOrder; bool bigEndian;
         if (!ReadGimFormat(gimFile, out format, out pixelOrder, out bigEndian))
         { log("[warn] " + Path.GetFileName(gimFile) + ": unrecognised gim header, using ps3 defaults"); return " -rcops3 -bpp32 -N"; }

         string flags = GetGimConvFlags(format, pixelOrder, bigEndian);
         if (flags == "")
         {
            log("[warn] " + Path.GetFileName(gimFile) + ": format " + format + " not re-encodable, using full colour");
            return (bigEndian ? " -rcops3" : "") + (pixelOrder == 0 ? " -N" : "") + " -bpp32";
         }
         return flags;
      }

      // the GimConv flags for one exact format: byte order (-rcops3 = ps3 big-endian, added
      // to our GimConv.cfg), pixel format (-bppX) and storage order (-N = normal; GimConv's
      // default is psp 'faster' order). "" when the format has no encoder flag.
      private static string GetGimConvFlags(int format, int pixelOrder, bool bigEndian)
      {
         string flags = bigEndian ? " -rcops3" : "";
         if (pixelOrder == 0) flags += " -N";
         switch (format)
         {
            case (int)GimFormat.Rgba5650: return flags + " -bpp16p";
            case (int)GimFormat.Rgba5551: return flags + " -bpp16";
            case (int)GimFormat.Rgba4444: return flags + " -bpp16a";
            case (int)GimFormat.Rgba8888: return flags + " -bpp32";
            case (int)GimFormat.Index4: return flags + " -bpp4";
            case (int)GimFormat.Index8: return flags + " -bpp8";
            case (int)GimFormat.Dxt1: return flags + " -bppdxt1";
            case (int)GimFormat.Dxt3: return flags + " -bppdxt3";
            case (int)GimFormat.Dxt5: return flags + " -bppdxt5";
         }
         return "";
      }

      private static int ReadU16(byte[] data, int offset, bool bigEndian)
      {
         return bigEndian ? (data[offset] << 8) | data[offset + 1] : (data[offset + 1] << 8) | data[offset];
      }

      private static int ReadU32(byte[] data, int offset, bool bigEndian)
      {
         return bigEndian
            ? (data[offset] << 24) | (data[offset + 1] << 16) | (data[offset + 2] << 8) | data[offset + 3]
            : (data[offset + 3] << 24) | (data[offset + 2] << 16) | (data[offset + 1] << 8) | data[offset];
      }

      // an edited wav -> encode back onto its vag channels (one vag per channel)
      private static void ConvertEditedSounds(RcoJob job, Action<string> log)
      {
         foreach (KeyValuePair<string, List<string>> sound in GroupVagChannels(job.DumpDir))
         {
            string wavFile = Path.Combine(job.DumpDir, sound.Key + ".wav");
            if (!File.Exists(wavFile) || !IsEdited(wavFile)) continue;

            EncodeWavToVag(wavFile, sound.Value, log);
            log("  converted edited " + sound.Key + ".wav back to vag");
         }
      }

      // snd_cursor.ch0.vag + snd_cursor.ch1.vag -> "snd_cursor" => [ch0, ch1]
      private static SortedDictionary<string, List<string>> GroupVagChannels(string dumpDir)
      {
         var groups = new SortedDictionary<string, List<string>>();
         foreach (string vagFile in Directory.GetFiles(dumpDir, "*.vag"))
         {
            string baseName = Path.GetFileNameWithoutExtension(vagFile);         // snd_cursor.ch0
            int channelMarker = baseName.LastIndexOf(".ch");
            if (channelMarker >= 0) baseName = baseName.Substring(0, channelMarker);
            if (!groups.ContainsKey(baseName)) groups[baseName] = new List<string>();
            groups[baseName].Add(vagFile);
         }
         foreach (List<string> channels in groups.Values) channels.Sort();
         return groups;
      }

      private static bool ReadHeaderCompression(string dumpOutput)
      {
         // dump prints "    Compression = 0x1" in its header info block
         int position = dumpOutput.IndexOf("Compression = 0x");
         if (position < 0) return true;
         return dumpOutput[position + "Compression = 0x".Length] != '0';
      }

      private static string Quote(string argument) { return "\"" + argument + "\""; }

      private static string QuoteAll(List<string> arguments)
      {
         return "\"" + string.Join("\" \"", arguments.ToArray()) + "\"";
      }

      private static string LastLines(string text, int count)
      {
         string[] lines = text.TrimEnd().Split('\n');
         int start = Math.Max(0, lines.Length - count);
         return string.Join("\n", lines, start, lines.Length - start);
      }

      // runs a tool silently, capturing stdout+stderr without pipe deadlocks
      private static string Run(string exePath, string arguments, string workingDir, out int exitCode)
      {
         var startInfo = new ProcessStartInfo(exePath, arguments)
         {
            WorkingDirectory = workingDir,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
         };
         var output = new StringBuilder();
         using (var process = new Process { StartInfo = startInfo })
         {
            process.OutputDataReceived += (sender, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
            process.ErrorDataReceived += (sender, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();
            exitCode = process.ExitCode;
         }
         return output.ToString();
      }
   }
}
