using System;
using System.Diagnostics;
using System.IO;

namespace ThemeStudio
{
   // menu sounds have to be in the console's own .vag format, and nothing in the SDK converts to
   // it. rcomage does, and rco-studio already bundles it, so the same tool is reused here rather
   // than a second encoder being written.
   public static class SoundConvert
   {
      // a vag holds sound in blocks of 28 samples, and the encoder rejects anything that does not
      // fill whole blocks
      private const int SamplesPerBlock = 28;

      private static string rcomageExe { get { return Path.Combine(ToolRun.ToolsDir, "Rcomage", "rcomage.exe"); } }

      public static bool IsWav(string path)
      {
         return Path.GetExtension(path).Equals(".wav", StringComparison.OrdinalIgnoreCase);
      }

      // converts a wav to a vag beside the given destination. returns the file the theme should
      // use, which is the original when it was already a vag.
      public static string ToVag(string soundPath, string destinationDir, Action<string> log)
      {
         if (!IsWav(soundPath)) return soundPath;
         if (!File.Exists(rcomageExe)) throw new IOException("rcomage.exe is missing from " + ToolRun.ToolsDir);

         string vagPath = Path.Combine(destinationDir, Path.GetFileNameWithoutExtension(soundPath) + ".vag");
         string paddedPath = "";
         string encodeFrom = soundPath;

         try {
            paddedPath = padToWholeBlocks(soundPath, destinationDir, log);
            if (paddedPath.Length > 0) encodeFrom = paddedPath;

            int exitCode;
            string output = ToolRun.Run(rcomageExe, "vagenc \"" + encodeFrom + "\" \"" + vagPath + "\"", out exitCode);
            if (exitCode != 0 || !File.Exists(vagPath))
               throw new IOException("could not convert " + Path.GetFileName(soundPath) + " to a console sound: " + output.Trim());

            log("converted " + Path.GetFileName(soundPath) + " to " + Path.GetFileName(vagPath));
            return vagPath;
         } finally {
            if (paddedPath.Length > 0 && File.Exists(paddedPath)) File.Delete(paddedPath);
         }
      }

      // the same sound with a few silent samples on the end, written beside the build rather than
      // over the user's own file. the padding is at most 27 samples, well under a millisecond.
      private static string padToWholeBlocks(string wavPath, string destinationDir, Action<string> log)
      {
         int dataOffset, dataLength, bytesPerFrame;
         if (!readWavShape(wavPath, out dataOffset, out dataLength, out bytesPerFrame)) return "";
         if (bytesPerFrame <= 0) return "";

         int leftOver = (dataLength / bytesPerFrame) % SamplesPerBlock;
         if (leftOver == 0) return "";

         int extraBytes = (SamplesPerBlock - leftOver) * bytesPerFrame;
         string paddedPath = Path.Combine(destinationDir, Path.GetFileNameWithoutExtension(wavPath) + ".padded.wav");

         byte[] original = File.ReadAllBytes(wavPath);
         using (var writer = new BinaryWriter(File.Create(paddedPath))) {
            writer.Write(original, 0, dataOffset);
            writer.Write(original, dataOffset, dataLength);
            writer.Write(new byte[extraBytes]);          // 16-bit silence is zeroes

            writer.Seek(4, SeekOrigin.Begin);            // riff size
            writer.Write(dataOffset + dataLength + extraBytes - 8);
            writer.Seek(dataOffset - 4, SeekOrigin.Begin);   // data chunk size
            writer.Write(dataLength + extraBytes);
         }
         log("padded " + Path.GetFileName(wavPath) + " with " + (SamplesPerBlock - leftOver) + " silent samples");
         return paddedPath;
      }

      private static bool readWavShape(string path, out int dataOffset, out int dataLength, out int bytesPerFrame)
      {
         dataOffset = dataLength = bytesPerFrame = 0;
         using (var file = File.OpenRead(path))
         using (var reader = new BinaryReader(file)) {
            if (file.Length < 12 || new string(reader.ReadChars(4)) != "RIFF") return false;
            reader.ReadInt32();
            if (new string(reader.ReadChars(4)) != "WAVE") return false;

            int channels = 0, bitsPerSample = 0;
            while (file.Position + 8 <= file.Length) {
               string chunk = new string(reader.ReadChars(4));
               int size = reader.ReadInt32();
               // the size comes out of the file, so it can be anything. a negative one would walk
               // the read position backwards and loop here for ever; one past the end would make
               // the caller read bytes that are not there.
               if (size < 0 || file.Position + size > file.Length) return false;

               if (chunk == "fmt ") {
                  if (size < 16) return false;
                  reader.ReadInt16();                    // format
                  channels = reader.ReadInt16();
                  reader.ReadInt32();                    // samples per second
                  reader.ReadInt32();                    // bytes per second
                  reader.ReadInt16();                    // block alignment
                  bitsPerSample = reader.ReadInt16();
                  file.Position += size - 16;
               } else if (chunk == "data") {
                  if (channels <= 0 || bitsPerSample <= 0) return false;   // data before fmt
                  dataOffset = (int)file.Position;
                  dataLength = size;
                  bytesPerFrame = channels * (bitsPerSample / 8);
                  return bytesPerFrame > 0;
               } else {
                  file.Position += size + (size & 1);
               }
            }
         }
         return false;
      }

   }
}
