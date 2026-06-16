using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace RenpyToPs3.RenPy
{
    // Builds a single-file .rpk bundle: compiles the whole game to bytecode (which also yields
    // the GUI manifest and the game's native resolution), converts every asset to a
    // PS3-playable format via ffmpeg, and writes manifest + game.gui + game.rbc + assets.
    //
    // Asset scaling is UNIFORM: one factor (target max edge / native resolution) applied to
    // every image and video, so relative sizes in the game's native coordinate space are
    // preserved exactly. The factor is recorded in game.gui as asset_scale (omitted when 1.0)
    // so the player can map texture pixels back to native pixels. When the native resolution
    // is unknown (no config.screen_width), falls back to a per-image max-edge cap.
    public static class Packer
    {
        private static readonly HashSet<string> ImageExt = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".tga" };
        private static readonly HashSet<string> AudioExt = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ".mp3", ".ogg", ".oga", ".wav", ".opus", ".m4a", ".aac", ".flac" };
        private static readonly HashSet<string> VideoExt = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ".webm", ".mpg", ".mpeg", ".avi", ".mp4", ".ogv", ".mkv", ".mov" };
        private static readonly HashSet<string> FontExt = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ".ttf", ".otf" };
        private static readonly HashSet<string> SkipExt = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ".rpa", ".rpyc", ".rpy", ".rpyb", ".rpymc", ".py", ".pyc", ".txt", ".json", ".ico", ".md" };

        // Bump whenever the asset CONVERSION logic changes (ffmpeg presets in Ffmpeg.cs, the
        // passthrough rules below, the output container choice, etc.). It is part of every cache key,
        // so bumping it invalidates the whole on-disk cache and forces a clean re-encode. This is the
        // backstop that stops a stale cached asset ever being served after a logic change.
        private const int CacheVersion = 1;

        public static int Pack(string gameDir, string outRpk, Ffmpeg ff, int maxDim, bool asciiText)
        {
            return Pack(gameDir, outRpk, ff, maxDim, asciiText, true, false);
        }

        public static int Pack(string gameDir, string outRpk, Ffmpeg ff, int maxDim, bool asciiText, bool useCache, bool clearCache)
        {
            string staging = Path.Combine(Path.GetTempPath(), "rpk_stage_" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(staging);

            // Persistent asset cache (next to the tool). The key (computed per asset in Convert) folds
            // in: CacheVersion + the ffmpeg binary's identity + in/out extension + scaling params +
            // a SHA-256 of the SOURCE bytes. So a cache hit is only possible when the exact same source
            // would be converted the exact same way -- any edit to the asset, a different --max, a new
            // ffmpeg, or a CacheVersion bump all change the key and force a fresh encode. `--no-cache`
            // bypasses it entirely (and also disables passthrough), `--clear-cache` wipes it first.
            string cacheDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "asset-cache");
            string ffFp = FfmpegFingerprint(ff);
            if (useCache)
            {
                try
                {
                    if (clearCache && Directory.Exists(cacheDir)) Directory.Delete(cacheDir, true);
                    Directory.CreateDirectory(cacheDir);
                }
                catch (Exception ex) { Console.WriteLine("  (cache disabled: " + ex.Message + ")"); useCache = false; }
            }
            try
            {
                // 1) Compile the whole game first: bytecode + GUI manifest + native resolution.
                List<IList> units = new List<IList>();
                foreach (string rpyc in Directory.GetFiles(gameDir, "*.rpyc", SearchOption.TopDirectoryOnly))
                {
                    try { IList l = RpycFile.LoadStatements(rpyc); if (l != null) units.Add(l); }
                    catch (Exception ex) { Console.Error.WriteLine("  (skip) " + Path.GetFileName(rpyc) + ": " + ex.Message); }
                }
                IrProgram prog = Compiler.CompileUnits(units, asciiText);
                // Build the GUI manifest BEFORE serialising bytecode: the manifest compiles some
                // expressions (e.g. side-image ConditionSwitch conditions) via prog.CompileExpr, which
                // appends to prog.Exprs -- those must be present when Bytecode.Write serialises the expr
                // section, or the exprIds the manifest emits would dangle.
                string gui = GuiManifest.Build(prog);
                byte[] rbc = Bytecode.Write(prog);

                int nativeW = ParseGuiInt(gui, "native_w");
                int nativeH = ParseGuiInt(gui, "native_h");
                bool uniform = nativeW > 0 && nativeH > 0;
                double factor = 1.0;
                if (uniform)
                    factor = Math.Min(1.0, Math.Min((double)maxDim / nativeW, (double)maxDim / nativeH));
                if (uniform && factor < 1.0)
                    gui += "asset_scale=" + factor.ToString("0.######", System.Globalization.CultureInfo.InvariantCulture) + "\n";
                Console.WriteLine(uniform
                    ? "scaling: uniform x" + factor.ToString("0.###") + " (native " + nativeW + "x" + nativeH + ", max edge " + maxDim + ")"
                    : "scaling: per-image max-edge cap " + maxDim + " (native resolution unknown)");

                // 2) Gather assets: RPA entries first, then loose files (loose overrides archived).
                Dictionary<string, byte[]> assets = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
                foreach (string rpa in Directory.GetFiles(gameDir, "*.rpa", SearchOption.TopDirectoryOnly))
                {
                    RpaArchive arc = new RpaArchive(rpa);
                    foreach (KeyValuePair<string, List<RpaArchive.Segment>> kv in arc.ReadIndex())
                        assets[kv.Key] = arc.ReadFile(kv.Value);
                }
                foreach (string f in Directory.GetFiles(gameDir, "*", SearchOption.AllDirectories))
                {
                    string ext = Path.GetExtension(f);
                    if (SkipExt.Contains(ext)) continue;
                    if (ImageExt.Contains(ext) || AudioExt.Contains(ext) || VideoExt.Contains(ext) || FontExt.Contains(ext))
                        assets[RelativePath(gameDir, f)] = File.ReadAllBytes(f);
                }

                // 3) Convert in parallel (each job gets its own staging file names; ffmpeg is an
                // external process, so the only shared state is the result collection below).
                List<RpkEntry> packed = new List<RpkEntry>();
                List<string> failed = new List<string>();
                int images = 0, audio = 0, video = 0, fonts = 0, skipped = 0;
                int cacheHits = 0, encoded = 0, passed = 0;   // where each converted asset came from
                long srcBytes = 0, dstBytes = 0;
                int done = 0, total = assets.Count, jobId = 0;
                object sync = new object();

                List<KeyValuePair<string, byte[]>> work = new List<KeyValuePair<string, byte[]>>(assets);
                ParallelOptions po = new ParallelOptions { MaxDegreeOfParallelism = Environment.ProcessorCount };
                Parallel.ForEach(work, po, kv =>
                {
                    string name = kv.Key;
                    byte[] data = kv.Value;
                    string ext = Path.GetExtension(name);
                    int id = Interlocked.Increment(ref jobId);

                    string outName = null;
                    byte[] outBytes = null;
                    string fail = null;
                    int kind = -1;   // 0 img, 1 audio, 2 video, 3 font, 4 skipped
                    int via = -1;    // how a converted asset was produced: 0 encoded, 1 cache, 2 passthrough

                    try
                    {
                        if (ImageExt.Contains(ext))
                        {
                            kind = 0;
                            string outExt = ext.Equals(".jpg", StringComparison.OrdinalIgnoreCase) || ext.Equals(".jpeg", StringComparison.OrdinalIgnoreCase) ? ".jpg" : ".png";
                            string err;
                            if (Convert(ff, data, ext, outExt, staging, id, uniform, factor, maxDim, cacheDir, useCache, ffFp, out outBytes, out err, out via)) outName = ChangeExt(name, outExt);
                            else fail = name + " : " + err;
                        }
                        else if (AudioExt.Contains(ext))
                        {
                            kind = 1;
                            string err;
                            if (Convert(ff, data, ext, ".ogg", staging, id, uniform, factor, maxDim, cacheDir, useCache, ffFp, out outBytes, out err, out via)) outName = ChangeExt(name, ".ogg");
                            else fail = name + " : " + err;
                        }
                        else if (VideoExt.Contains(ext))
                        {
                            kind = 2;
                            string err;
                            if (Convert(ff, data, ext, ".mp4", staging, id, uniform, factor, maxDim, cacheDir, useCache, ffFp, out outBytes, out err, out via)) outName = ChangeExt(name, ".mp4");
                            else fail = name + " : " + err;
                        }
                        else if (FontExt.Contains(ext)) { kind = 3; outName = name; outBytes = data; }
                        else kind = 4;
                    }
                    catch (Exception ex) { fail = name + " : " + ex.Message; }

                    lock (sync)
                    {
                        srcBytes += data.Length;
                        if (outName != null)
                        {
                            packed.Add(new RpkEntry(outName, outBytes));
                            dstBytes += outBytes.Length;
                            if (kind == 0) images++; else if (kind == 1) audio++; else if (kind == 2) video++; else if (kind == 3) fonts++;
                            if (via == 0) encoded++; else if (via == 1) cacheHits++; else if (via == 2) passed++;
                        }
                        else if (fail != null) failed.Add(fail);
                        else if (kind == 4) skipped++;
                        done++;
                        if (done % 25 == 0 || done == total)
                            Console.WriteLine("  [" + done + "/" + total + "] converting assets...");
                    }
                });

                // Deterministic bundle: assets sorted by name (parallel completion order is not).
                packed.Sort((x, y) => string.CompareOrdinal(x.Name, y.Name));

                // 4) Manifest.
                DirectoryInfo di = new DirectoryInfo(Path.GetFullPath(gameDir).TrimEnd(Path.DirectorySeparatorChar));
                string title = di.Parent != null ? di.Parent.Name : Path.GetFileName(gameDir);
                StringBuilder man = new StringBuilder();
                man.AppendLine("title: " + title);
                man.AppendLine("entry: " + (prog.Labels.ContainsKey("start") ? "start" : "(no start label)"));
                man.AppendLine("rpk_version: " + Rpk.Version);
                man.AppendLine("images: " + images);
                man.AppendLine("audio: " + audio);
                man.AppendLine("video: " + video);
                man.AppendLine("fonts: " + fonts);

                // 5) Assemble .rpk.
                List<RpkEntry> entries = new List<RpkEntry>();
                entries.Add(new RpkEntry("manifest", Encoding.UTF8.GetBytes(man.ToString())));
                entries.Add(new RpkEntry("game.gui", Encoding.UTF8.GetBytes(gui)));
                entries.Add(new RpkEntry("game.rbc", rbc));
                foreach (RpkEntry a in packed) entries.Add(new RpkEntry("assets/" + a.Name, a.Data));
                Rpk.Write(outRpk, entries);

                long rpkSize = new FileInfo(outRpk).Length;
                int unsup = new HashSet<string>(prog.Unsupported).Count;
                int unres = new HashSet<string>(prog.Unresolved).Count;
                Console.WriteLine("");
                Console.WriteLine("== pack summary ==");
                Console.WriteLine("images: " + images + "  audio: " + audio + "  video: " + video + "  fonts: " + fonts + "  skipped: " + skipped);
                Console.WriteLine(useCache
                    ? "asset cache: " + cacheHits + " reused, " + passed + " passthrough, " + encoded + " encoded (dir: " + cacheDir + ")"
                    : "asset cache: DISABLED (--no-cache) -- everything re-encoded fresh");
                Console.WriteLine("bytecode: " + prog.Code.Count + " instrs (" + rbc.Length + " bytes); labels: " + prog.Labels.Count);
                Console.WriteLine("unsupported nodes: " + unsup + "  unresolved targets: " + unres);
                PrintNotes(prog);
                long pct = srcBytes > 0 ? (100L * dstBytes / srcBytes) : 0;
                Console.WriteLine("asset bytes: " + srcBytes + " -> " + dstBytes + "  (" + pct + "% of source)");
                Console.WriteLine("failed conversions: " + failed.Count);
                failed.Sort(StringComparer.Ordinal);
                for (int k = 0; k < failed.Count && k < 15; k++) Console.WriteLine("    " + failed[k]);
                Console.WriteLine("");
                Console.WriteLine("wrote " + outRpk + " (" + rpkSize + " bytes, " + entries.Count + " entries)");
                return failed.Count == 0 ? 0 : 2;
            }
            finally
            {
                try { Directory.Delete(staging, true); } catch { }
            }
        }

        private static void PrintNotes(IrProgram prog)
        {
            List<string> notes = new List<string>(new HashSet<string>(prog.Notes));
            notes.Sort(StringComparer.Ordinal);
            Console.WriteLine("fidelity notes: " + (notes.Count == 0 ? "none" : notes.Count.ToString()));
            for (int k = 0; k < notes.Count && k < 15; k++) Console.WriteLine("    " + notes[k]);
        }

        private static bool Convert(Ffmpeg ff, byte[] src, string inExt, string outExt, string staging, int id,
                                    bool uniform, double factor, int maxDim, string cacheDir, bool useCache, string ffFp,
                                    out byte[] outBytes, out string err, out int via)
        {
            outBytes = null; err = null; via = 0;

            // Cache key folds in EVERYTHING that can change the output (see CacheVersion). A hit is only
            // possible for the exact same source bytes + same conversion params + same ffmpeg + same
            // logic version -- so it can never serve a result that a fresh encode wouldn't reproduce.
            string cachePath = null;
            if (useCache)
            {
                string key = CacheKey(src, inExt, outExt, uniform, factor, maxDim, ffFp);
                cachePath = Path.Combine(cacheDir, key + outExt);
                try { if (File.Exists(cachePath)) { outBytes = File.ReadAllBytes(cachePath); via = 1; return true; } }
                catch { /* unreadable cache entry -> fall through and re-encode */ outBytes = null; }
            }

            // Passthrough: an already-PS3-safe image needing no resize would only be re-encoded to the
            // same form (and a JPEG would lose quality), so skip ffmpeg and keep the bytes. Conservative
            // -- plain 8-bit non-interlaced PNG and baseline JPEG only. Off under --no-cache.
            if (useCache && CanPassThrough(src, inExt, outExt, uniform, factor, maxDim))
            {
                outBytes = src; via = 2;
                StoreCache(cachePath, outBytes);
                return true;
            }

            string inp = Path.Combine(staging, "in" + id + inExt);
            string outp = Path.Combine(staging, "out" + id + outExt);
            File.WriteAllBytes(inp, src);

            bool ok;
            if (outExt == ".ogg") ok = ff.Audio(inp, outp, out err);
            else if (outExt == ".mp4") ok = uniform ? ff.VideoScaled(inp, outp, factor, out err) : ff.Video(inp, outp, maxDim, maxDim, out err);
            else ok = uniform ? ff.ImageScaled(inp, outp, factor, out err) : ff.Image(inp, outp, maxDim, maxDim, out err);

            if (ok && File.Exists(outp)) outBytes = File.ReadAllBytes(outp);
            else if (string.IsNullOrEmpty(err)) err = "no output produced";
            else { int nl = err.IndexOf('\n'); if (nl >= 0) err = err.Substring(0, nl); }

            try { File.Delete(inp); } catch { }
            try { File.Delete(outp); } catch { }
            if (outBytes != null) { via = 0; StoreCache(cachePath, outBytes); return true; }
            return false;
        }

        // A fingerprint of the ffmpeg binary (size + last-write time), folded into every cache key so
        // swapping ffmpeg invalidates the cache. Falls back to the path string if it can't be stat'd.
        private static string FfmpegFingerprint(Ffmpeg ff)
        {
            try { FileInfo fi = new FileInfo(ff.Path); if (fi.Exists) return fi.Length + ":" + fi.LastWriteTimeUtc.Ticks; }
            catch { }
            return ff.Path ?? "";
        }

        // SHA-256 over (params header || source bytes). The header carries the cache version, ffmpeg
        // fingerprint, in/out extensions and scaling params -- so identical content converted a
        // different way gets a different key.
        private static string CacheKey(byte[] src, string inExt, string outExt, bool uniform, double factor, int maxDim, string ffFp)
        {
            string hdr = "v" + CacheVersion + "|" + ffFp + "|" + inExt.ToLowerInvariant() + "|" + outExt + "|"
                       + (uniform ? "U" : "P") + "|"
                       + factor.ToString("0.######", System.Globalization.CultureInfo.InvariantCulture) + "|" + maxDim + "|";
            using (SHA256 sha = SHA256.Create())
            {
                byte[] hb = Encoding.UTF8.GetBytes(hdr);
                sha.TransformBlock(hb, 0, hb.Length, null, 0);
                sha.TransformFinalBlock(src, 0, src.Length);
                return BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
            }
        }

        // Atomic best-effort cache write (temp file + move) so a crash or two racing threads never leave
        // a half-written entry that a later run would trust.
        private static void StoreCache(string path, byte[] data)
        {
            if (path == null) return;
            try
            {
                if (File.Exists(path)) return;
                string tmp = path + ".tmp" + Guid.NewGuid().ToString("N").Substring(0, 8);
                File.WriteAllBytes(tmp, data);
                try { if (!File.Exists(path)) File.Move(tmp, path); else File.Delete(tmp); }
                catch { try { File.Delete(tmp); } catch { } }
            }
            catch { /* cache is an optimization; never fail the pack over it */ }
        }

        // True only when `src` is an image already in a PS3-safe encoding that needs no resize, so its
        // bytes can be used verbatim instead of round-tripping through ffmpeg. Deliberately strict.
        private static bool CanPassThrough(byte[] src, string inExt, string outExt, bool uniform, double factor, int maxDim)
        {
            inExt = inExt.ToLowerInvariant();
            int w, h;
            if (inExt == ".png" && outExt == ".png") { if (!PngPlain(src, out w, out h)) return false; }
            else if ((inExt == ".jpg" || inExt == ".jpeg") && outExt == ".jpg") { if (!JpgBaseline(src, out w, out h)) return false; }
            else return false;
            if (uniform) return factor >= 1.0;                 // uniform factor 1.0 => no scaling at all
            return w > 0 && h > 0 && w <= maxDim && h <= maxDim;
        }

        // Plain PNG: 8-bit, truecolour (RGB) or truecolour+alpha (RGBA), non-interlaced. (What ffmpeg
        // would emit anyway.) Paletted/16-bit/interlaced/grayscale are rejected to stay safe.
        private static bool PngPlain(byte[] b, out int w, out int h)
        {
            w = 0; h = 0;
            if (b.Length < 33) return false;
            byte[] sig = { 137, 80, 78, 71, 13, 10, 26, 10 };
            for (int i = 0; i < 8; i++) if (b[i] != sig[i]) return false;
            if (!(b[12] == 'I' && b[13] == 'H' && b[14] == 'D' && b[15] == 'R')) return false;
            w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
            h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
            int bitDepth = b[24], colorType = b[25], interlace = b[28];
            if (bitDepth != 8) return false;
            if (colorType != 2 && colorType != 6) return false;
            if (interlace != 0) return false;
            return w > 0 && h > 0;
        }

        // Baseline JPEG (SOF0), 8-bit, 1 or 3 components. Progressive/arithmetic/other SOF variants and
        // anything malformed are rejected (the PS3 jpgdec wants baseline).
        private static bool JpgBaseline(byte[] b, out int w, out int h)
        {
            w = 0; h = 0;
            if (b.Length < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;
            int p = 2;
            while (p + 2 <= b.Length)
            {
                if (b[p] != 0xFF) return false;
                int marker = b[p + 1]; p += 2;
                if (marker == 0xD9) return false;                       // EOI before SOF0
                if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; // standalone, no length
                if (p + 2 > b.Length) return false;
                int len = (b[p] << 8) | b[p + 1];
                if (len < 2 || p + len > b.Length) return false;
                if (marker == 0xC0)                                     // SOF0 = baseline
                {
                    if (p + 8 > b.Length) return false;
                    int prec = b[p + 2];
                    h = (b[p + 3] << 8) | b[p + 4];
                    w = (b[p + 5] << 8) | b[p + 6];
                    int comps = b[p + 7];
                    if (prec != 8) return false;
                    if (comps != 1 && comps != 3) return false;
                    return w > 0 && h > 0;
                }
                // any other Start-Of-Frame (progressive/arithmetic/lossless) or reaching scan data first
                if (marker == 0xC1 || marker == 0xC2 || marker == 0xC3 || (marker >= 0xC5 && marker <= 0xCF && marker != 0xC8) || marker == 0xDA)
                    return false;
                p += len;
            }
            return false;
        }

        // Reads "key=NNN" from the game.gui text; 0 if absent.
        private static int ParseGuiInt(string gui, string key)
        {
            foreach (string line in gui.Split('\n'))
            {
                if (!line.StartsWith(key + "=", StringComparison.Ordinal)) continue;
                int v;
                if (int.TryParse(line.Substring(key.Length + 1).Trim(), out v)) return v;
            }
            return 0;
        }

        private static string RelativePath(string baseDir, string full)
        {
            string b = Path.GetFullPath(baseDir);
            if (!b.EndsWith(Path.DirectorySeparatorChar.ToString())) b += Path.DirectorySeparatorChar;
            string f = Path.GetFullPath(full);
            string rel = f.StartsWith(b, StringComparison.OrdinalIgnoreCase) ? f.Substring(b.Length) : f;
            return rel.Replace('\\', '/');
        }

        private static string ChangeExt(string name, string ext)
        {
            int slash = name.LastIndexOf('/');
            int dot = name.LastIndexOf('.');
            if (dot <= slash) return name + ext;
            return name.Substring(0, dot) + ext;
        }
    }
}
