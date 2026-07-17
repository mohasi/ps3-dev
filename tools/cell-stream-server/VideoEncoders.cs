using System;
using System.Collections.Generic;
using System.Diagnostics;
using Microsoft.Win32;

namespace CellStreamServer
{
   internal enum EncoderKind { Nvenc, Amf, QuickSync, Cpu }

   internal sealed class VideoEncoder
   {
      public EncoderKind Kind;
      public string Name;              // what the window shows
      public string ProbeArguments;    // encodes one black frame: if that works, this PC has the hardware

      public override string ToString() { return Name; }   // what the dropdown shows
   }

   // the encoders we know how to drive, best first. which of them this PC can actually run is found out
   // once, at start-up, by asking each to encode a single frame - a machine with no NVIDIA card should
   // never be offered nvenc, let alone waste a second failing it on every stream.
   internal static class VideoEncoders
   {
      private const string SettingsKey = @"Software\CellStreamServer";
      private const string ChosenValueName = "Encoder";
      private const int ProbeTimeoutMs = 15000;
      private const string ProbeSource = "-hide_banner -loglevel error -f lavfi -i color=c=black:s=320x240:d=0.1 -frames:v 1";

      public static readonly VideoEncoder[] Ladder =
      {
         new VideoEncoder { Kind = EncoderKind.Nvenc, Name = "NVIDIA GPU (nvenc)",
                            ProbeArguments = ProbeSource + " -c:v h264_nvenc -f null -" },
         new VideoEncoder { Kind = EncoderKind.Amf, Name = "AMD GPU (amf)",
                            ProbeArguments = ProbeSource + " -c:v h264_amf -f null -" },
         new VideoEncoder { Kind = EncoderKind.QuickSync, Name = "Intel GPU (Quick Sync)",
                            ProbeArguments = ProbeSource + " -c:v h264_qsv -f null -" },
         new VideoEncoder { Kind = EncoderKind.Cpu, Name = "CPU (x264 - expect reduced fps)",
                            ProbeArguments = ProbeSource + " -c:v libx264 -f null -" }
      };

      public static List<VideoEncoder> DetectAvailable(string ffmpegPath)
      {
         var available = new List<VideoEncoder>();
         foreach (VideoEncoder encoder in Ladder)
            if (CanRun(ffmpegPath, encoder)) available.Add(encoder);

         Server.Log(available.Count == 0 ? "encoders: none of them work on this PC"
                                         : "encoders: " + string.Join(", ", available.ConvertAll(e => e.Name).ToArray()));
         return available;
      }

      private static bool CanRun(string ffmpegPath, VideoEncoder encoder)
      {
         var startInfo = new ProcessStartInfo(ffmpegPath, encoder.ProbeArguments)
         {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
         };

         try
         {
            using (Process probe = Process.Start(startInfo))
            {
               ChildProcessJob.Assign(probe);      // reaped with the server if it dies mid-probe
               string errorText = probe.StandardError.ReadToEnd();   // a probe that fills its pipe would otherwise hang
               probe.StandardOutput.ReadToEnd();
               if (!probe.WaitForExit(ProbeTimeoutMs)) { try { probe.Kill(); } catch { } return false; }
               if (probe.ExitCode == 0) return true;

               // hardware the PC lacks is expected to fail here - but so is a too-old driver, and that one a
               // user needs told. log the reason (last real line of ffmpeg's output) so it isn't a silent no.
               Server.Log("encoders: " + encoder.Name + " unavailable" + describeProbeFailure(errorText));
               return false;
            }
         }
         catch (Exception exception)
         {
            Server.Log("encoders: could not test " + encoder.Name + ": " + exception.Message);
            return false;
         }
      }

      // ffmpeg's boilerplate trailer ("nothing was written", "conversion failed") hides the real cause,
      // which comes first (driver too old, no device, codec not built in). return the first meaningful
      // line as ": <reason>", skipping the generic trailer; empty if it said nothing useful.
      private static readonly string[] GenericTrailers = { "Nothing was written", "Conversion failed", "Error opening output", "frame=", "[out#" };

      private static string describeProbeFailure(string errorText)
      {
         if (string.IsNullOrEmpty(errorText)) return "";
         foreach (string raw in errorText.Replace("\r", "").Split('\n'))
         {
            string line = raw.Trim();
            if (line.Length == 0) continue;
            bool generic = false;
            foreach (string trailer in GenericTrailers) if (line.IndexOf(trailer, StringComparison.OrdinalIgnoreCase) >= 0) { generic = true; break; }
            if (!generic) return ": " + line;
         }
         return "";
      }

      // the chosen encoder is remembered, so the next run starts on the one that worked
      public static VideoEncoder LoadChoice(List<VideoEncoder> available)
      {
         if (available.Count == 0) return null;
         try
         {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(SettingsKey))
            {
               string saved = key == null ? null : key.GetValue(ChosenValueName) as string;
               if (saved != null)
                  foreach (VideoEncoder encoder in available)
                     if (encoder.Kind.ToString() == saved) return encoder;
            }
         }
         catch { }
         return available[0];   // nothing remembered: the best one this PC has
      }

      public static void SaveChoice(VideoEncoder encoder)
      {
         try
         {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(SettingsKey))
               if (key != null) key.SetValue(ChosenValueName, encoder.Kind.ToString());
         }
         catch (Exception exception)
         {
            Server.Log("encoders: could not remember the choice: " + exception.Message);
         }
      }
   }
}
