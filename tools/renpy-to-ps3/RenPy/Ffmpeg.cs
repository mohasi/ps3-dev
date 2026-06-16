using System;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace RenpyToPs3.RenPy
{
    // Thin wrapper over an ffmpeg binary (bundled next to the tool, or on PATH).
    // PS3-tuned presets: image downscale-only fit; audio OGG Vorbis q4 48k stereo;
    // video H.264 Main/L4.0 yuv420p + AAC-LC in MP4.
    public sealed class Ffmpeg
    {
        public readonly string Path;

        public Ffmpeg(string explicitPath)
        {
            string p = Resolve(explicitPath);
            if (p == null)
                throw new FileNotFoundException(
                    "ffmpeg not found. Put ffmpeg(.exe) next to the tool, add it to PATH, or pass --ffmpeg <path>.");
            Path = p;
        }

        private static string ExeName()
        {
            return System.IO.Path.DirectorySeparatorChar == '\\' ? "ffmpeg.exe" : "ffmpeg";
        }

        private static string Resolve(string p)
        {
            if (!string.IsNullOrEmpty(p) && File.Exists(p)) return p;
            string exe = ExeName();

            string local = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory, exe);
            if (File.Exists(local)) return local;

            string path = Environment.GetEnvironmentVariable("PATH");
            if (path != null)
                foreach (string dir in path.Split(new char[] { System.IO.Path.PathSeparator }))
                {
                    try { string c = System.IO.Path.Combine(dir.Trim(), exe); if (File.Exists(c)) return c; }
                    catch { /* malformed PATH entry */ }
                }
            return null;
        }

        private static string Quote(string a)
        {
            if (a.Length == 0) return "\"\"";
            if (a.IndexOf(' ') < 0 && a.IndexOf('"') < 0) return a;
            return "\"" + a.Replace("\"", "\\\"") + "\"";
        }

        public bool Run(string[] args, out string err, int timeoutMs)
        {
            StringBuilder cmd = new StringBuilder();
            for (int i = 0; i < args.Length; i++)
            {
                if (i > 0) cmd.Append(' ');
                cmd.Append(Quote(args[i]));
            }

            ProcessStartInfo psi = new ProcessStartInfo(Path, cmd.ToString());
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;

            StringBuilder errBuf = new StringBuilder();
            using (Process pr = new Process())
            {
                pr.StartInfo = psi;
                pr.ErrorDataReceived += delegate (object s, DataReceivedEventArgs e) { if (e.Data != null) errBuf.AppendLine(e.Data); };
                pr.OutputDataReceived += delegate (object s, DataReceivedEventArgs e) { };
                pr.Start();
                pr.BeginErrorReadLine();
                pr.BeginOutputReadLine();

                if (!pr.WaitForExit(timeoutMs))
                {
                    try { pr.Kill(); } catch { }
                    err = "ffmpeg timed out";
                    return false;
                }
                pr.WaitForExit(); // flush async readers
                err = errBuf.ToString();
                return pr.ExitCode == 0;
            }
        }

        public bool Run(string[] args, out string err) { return Run(args, out err, 600000); }

        private static string Scale(int maxW, int maxH)
        {
            return "scale='min(iw," + maxW + ")':'min(ih," + maxH + ")':force_original_aspect_ratio=decrease";
        }

        // Uniform downscale: every asset shrinks by the SAME factor, so relative sizes in the
        // game's native coordinate space are preserved exactly (a per-image max-edge cap would
        // shrink only the biggest assets and distort sprite/background ratios). Video keeps
        // even dimensions for yuv420p.
        private static string UniformScale(double factor, bool even)
        {
            string f = factor.ToString("0.######", System.Globalization.CultureInfo.InvariantCulture);
            return even
                ? "scale=trunc(iw*" + f + "/2)*2:trunc(ih*" + f + "/2)*2"
                : "scale=trunc(iw*" + f + "):trunc(ih*" + f + ")";
        }

        public bool Image(string inp, string outp, int maxW, int maxH, out string err)
        {
            return Run(new string[] { "-y", "-hide_banner", "-loglevel", "error", "-i", inp, "-vf", Scale(maxW, maxH), outp }, out err);
        }

        // factor >= 1 means "no resize" (still re-encodes to the target container/format).
        public bool ImageScaled(string inp, string outp, double factor, out string err)
        {
            if (factor >= 1.0)
                return Run(new string[] { "-y", "-hide_banner", "-loglevel", "error", "-i", inp, outp }, out err);
            return Run(new string[] { "-y", "-hide_banner", "-loglevel", "error", "-i", inp, "-vf", UniformScale(factor, false), outp }, out err);
        }

        public bool Audio(string inp, string outp, out string err)
        {
            return Run(new string[] { "-y", "-hide_banner", "-loglevel", "error", "-i", inp,
                "-c:a", "libvorbis", "-q:a", "4", "-ar", "48000", "-ac", "2", outp }, out err);
        }

        public bool Video(string inp, string outp, int maxW, int maxH, out string err)
        {
            return Run(new string[] { "-y", "-hide_banner", "-loglevel", "error", "-i", inp,
                "-c:v", "libx264", "-profile:v", "main", "-level", "4.0", "-pix_fmt", "yuv420p",
                "-preset", "veryfast", "-crf", "23", "-vf", Scale(maxW, maxH),
                "-c:a", "aac", "-b:a", "160k", "-movflags", "+faststart", outp }, out err, 1800000);
        }

        public bool VideoScaled(string inp, string outp, double factor, out string err)
        {
            // even-dimension scale is applied even at factor 1.0 (yuv420p requires it).
            double f = factor >= 1.0 ? 1.0 : factor;
            return Run(new string[] { "-y", "-hide_banner", "-loglevel", "error", "-i", inp,
                "-c:v", "libx264", "-profile:v", "main", "-level", "4.0", "-pix_fmt", "yuv420p",
                "-preset", "veryfast", "-crf", "23", "-vf", UniformScale(f, true),
                "-c:a", "aac", "-b:a", "160k", "-movflags", "+faststart", outp }, out err, 1800000);
        }
    }
}
