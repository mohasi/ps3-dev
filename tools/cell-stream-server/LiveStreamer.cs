using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace CellStreamServer
{
   // rung-2b live sender: captures the desktop and encodes H.264 with a spawned ffmpeg
   // (ddagrab = Windows' low-latency GPU screen capture), reads the raw Annex-B stream from
   // its output pipe, splits it into access units incrementally, and sends each one to the
   // client as it comes out of the encoder. tries each GPU encoder in turn (NVIDIA, Intel, AMD) and falls
   // back to the CPU, remembering whichever worked.
   internal sealed class LiveStreamer
   {
      private const int FirstFrameTimeoutMs = 5000;
      private const int EncoderExitWaitMs = 3000;   // Windows only hands the screen capture to one ffmpeg at a time
      private const int FailedStartsBeforeGivingUp = 3;

      // INTRA REFRESH: no keyframe bursts - every frame redraws a thin strip instead, sweeping across the
      // picture. Measured 59ms -> 39ms end-to-end, and frozen frames 287 -> 0.
      //
      // The numbers below are the whole story, and getting them wrong is what sank the first attempt. A FAST
      // sweep means a FAT intra strip in every frame; under tight CBR the encoder cannot afford it, codes it
      // coarsely, and you SEE a blur bar wiping across the picture. A slow sweep makes the strip a sliver
      // that costs almost nothing. Measured off the encoder's own output (per-strip frame differencing):
      // strip redraw strength (grey levels, lower = less visible): 0.75 at a 0.5s CBR sweep - that was the
      // visible bar. 0.02 at 4s VBR, 0.06 at 1s VBR, against a 0.02 no-intra-refresh baseline. So the RATE
      // CONTROL was most of the fix, not the sweep length: with headroom to borrow bits, even a 1s sweep is
      // ~10x below the bar we could see. 1s is chosen because the sweep is also what repairs a lost frame,
      // and a 4s sweep left visible damage on screen for 4s. Raise this if a faint bar ever shows.
      private const int RefreshSweepSeconds = 1;
      private const int RefreshKeyframeIntervalSeconds = 10;   // an anchor only; the sweep does the repairing
      private const int RefreshQpDelta = -2;
      private const int RefreshMaxRatePercent = 140;           // VBR headroom over the target bitrate
      private const int RefreshBufferMs = 250;

      private readonly Socket socket;
      private readonly string ffmpegPath;
      private readonly int fps, kbps, sendRateKbps;
      private readonly int outputWidth, outputHeight, sinfoLevel;
      private volatile bool streaming;
      private Thread pumpThread;
      private Process encoderProcess;
      private IPEndPoint currentTarget;
      private int failedStarts;

      // sendRateKbps caps how fast packets leave, independent of the video bitrate. it must stay
      // under what the link can actually carry (WiFi to the PS3 tops out ~22Mbps - its radio is
      // 802.11g), or a keyframe's burst overruns it and the picture freezes until the next one.
      public LiveStreamer(Socket socket, string ffmpegPath, int fps, int kbps, int outputWidth, int outputHeight, int sendRateKbps)
      {
         this.socket = socket;
         this.ffmpegPath = ffmpegPath;
         this.fps = fps;
         this.kbps = kbps;
         this.sendRateKbps = sendRateKbps;
         this.outputWidth = outputWidth;
         this.outputHeight = outputHeight;

         // the level tells the PS3 how much decoder memory to reserve, so it MUST match the level
         // the encoder actually writes into the stream - it picks its own from resolution/fps/bitrate
         // (720p60 comes out as 4.0, not the 3.2 the resolution alone suggests, and the mismatch
         // left the decoder mis-sized: it produced black). 4.2 covers everything we send, and
         // over-reserving is harmless.
         sinfoLevel = 42;
      }

      // the PS3 repeats PLAY until it hears back, so a repeat is not a new session: answer it and carry
      // on. restarting on every repeat meant the encoder hunt began again each time and never finished.
      public bool IsStreaming { get { return streaming; } }

      private bool IsStreamingTo(IPEndPoint target)
      {
         return streaming && target.Equals(currentTarget);
      }

      public void ResetFailures() { failedStarts = 0; }

      public void Start(IPEndPoint target)
      {
         if (IsStreamingTo(target)) { SendStreamInfo(target); return; }
         Stop();
         streaming = true;
         currentTarget = target;
         SendStreamInfo(target);   // answer the PS3 straight away: bringing an encoder up can take seconds
         pumpThread = new Thread(() => RunPump(target)) { IsBackground = true, Name = "live-pump" };
         pumpThread.Start();
      }

      // SINFO announces the source's frame rate, and whether this is an intra-refresh stream - which decides
      // how the PS3 handles a loss (hold the picture for the next keyframe, or decode on through the damage
      // and let the sweep clean it up). the PS3 configures its decoder from the stream's own SPS.
      private void SendStreamInfo(IPEndPoint target)
      {
         byte[] info = Encoding.ASCII.GetBytes("SINFO " + outputWidth + " " + outputHeight + " " + sinfoLevel + " 1 " + fps + " 1");
         for (int i = 0; i < 3; i++) socket.SendTo(info, target);
      }

      // waiting for ffmpeg to actually be gone matters: Windows hands the screen capture to one process at a
      // time, so a still-dying encoder makes the next one fail with "DDA ReleaseFrame failed" and no frames.
      public void Stop()
      {
         streaming = false;
         Process running = encoderProcess;
         if (running != null)
         {
            try { running.Kill(); } catch { }
            try { running.WaitForExit(EncoderExitWaitMs); } catch { }
         }
         if (pumpThread != null) { pumpThread.Join(2000); pumpThread = null; }
      }

      // rate control for the intra-refresh stream: VBR with headroom (maxrate) so the sweep strip can
      // borrow bits when a frame needs them, and a small VBV buffer (RefreshBufferMs) that keeps any single
      // frame - including the 10s anchor IDR - well under the receiver's per-frame limit.
      private string GetRateArguments()
      {
         return " -b:v " + kbps + "k -maxrate " + kbps * RefreshMaxRatePercent / 100 + "k -bufsize " +
                kbps * RefreshBufferMs / 1000 + "k -g " + RefreshKeyframeIntervalSeconds * fps + " -bf 0 -refs 1";
      }

      // frames per full sweep. every encoder spells intra refresh differently, but all of them need
      // B-frames off (set above): a sweep only works if each frame strictly follows the last.
      private int RefreshCycleFrames { get { return Math.Max(2, fps * RefreshSweepSeconds); } }

      private string GetQsvRefreshArguments()
      {
         return " -int_ref_type 1 -int_ref_cycle_size " + RefreshCycleFrames + " -int_ref_cycle_dist " + RefreshCycleFrames +
                " -int_ref_qp_delta " + RefreshQpDelta + " -recovery_point_sei 1";
      }

      // the CPU capture chain copies every full-size desktop frame out of the GPU before scaling
      // it - at 4K that alone caps the whole pipeline around 21fps (measured). the scale must
      // happen on the GPU, so only the small scaled frame crosses to the CPU.
      // out_color_matrix=bt709 matches the PS3's BT.709 color shader (its default, BT.601, shifts colors).
      private string GetCpuCaptureChain()
      {
         return " -filter_complex ddagrab=framerate=" + fps + ",hwdownload,format=bgra,scale=" + outputWidth + ":" + outputHeight +
                ":flags=lanczos:out_color_matrix=bt709:out_range=tv";
      }

      // Intel: keep the frame on the GPU from capture through scale into the encoder. out_range=limited
      // is essential - without it the desktop's full-range RGB becomes full-range YUV (yuvj420p) while
      // the PS3's shader expects limited range, so every colour lands wrong.
      //
      // async_depth=1 on the SCALER is a big latency win: the vpp_qsv filter defaults to holding 4
      // frames in flight (~67ms at 60fps) before it hands the oldest to the encoder. the -async_depth on
      // the encoder is a separate setting and does not touch the scaler's queue. 1 = no scaler backlog.
      //
      // tried and dropped: -max_dec_frame_buffering 1, to stop the PS3 holding a frame in its decoder.
      // it does nothing - the PS3 picks its own buffer count (cellVdec needs at least refs+1 = 2), and
      // never reads that field from the stream.
      //
      // tried and dropped, both no visible improvement: the scaler's high quality mode (scale_mode=hq)
      // for the 1.5x shrink, and a slower encoder preset - the slower preset also added real lag.
      private string BuildQsvArguments()
      {
         return "-hide_banner -loglevel warning -init_hw_device d3d11va=dx -init_hw_device qsv=qs@dx -filter_hw_device dx" +
                " -filter_complex ddagrab=framerate=" + fps + ",hwmap=derive_device=qsv,vpp_qsv=w=" + outputWidth +
                ":h=" + outputHeight + ":format=nv12:out_range=limited:async_depth=1" +
                " -c:v h264_qsv -preset medium -async_depth 1 -look_ahead 0" + GetRateArguments() + GetQsvRefreshArguments() +
                " -color_range tv -colorspace bt709 -forced_idr 1 -f h264 -flush_packets 1 pipe:1";
      }

      // NVIDIA: on a laptop with switchable graphics the desktop is driven by the Intel chip, so the d3d11
      // capture device is Intel. Given only that device, ffmpeg tries to DERIVE nvenc's CUDA device from the
      // Intel one and fails ("no encode device"). The fix is to hand nvenc its OWN cuda device (which only ever
      // enumerates the NVIDIA GPU): ddagrab captures on d3d11 (dx), the frame is scaled on the CPU (same chain
      // as the CPU encoder), and nvenc encodes on cuda (cu). -filter_hw_device dx keeps ddagrab on d3d11 while
      // the cuda device stays free for the encoder. -pix_fmt yuv420p matches the known-good CPU path's colours.
      private string BuildNvencArguments()
      {
         return "-hide_banner -loglevel warning -init_hw_device d3d11va=dx -init_hw_device cuda=cu -filter_hw_device dx" +
                GetCpuCaptureChain() +
                " -c:v h264_nvenc -preset p1 -tune ull -rc vbr -pix_fmt yuv420p" + GetRateArguments() +
                " -intra-refresh 1 -single-slice-intra-refresh 1" +
                " -color_range tv -colorspace bt709 -forced-idr 1 -f h264 -flush_packets 1 pipe:1";
      }

      // AMD: this build's ffmpeg has no D3D11 scaler (scale_d3d11 doesn't exist), so capture on the GPU then
      // scale on the CPU - the same chain the CPU encoder uses - and hand the frame to the AMD hardware
      // encoder. still AMD-hardware-unverified (no card here); if it doesn't take, the ladder falls through
      // to the CPU encoder as before.
      private string BuildAmfArguments()
      {
         return "-hide_banner -loglevel warning -init_hw_device d3d11va" + GetCpuCaptureChain() +
                " -c:v h264_amf -usage ultralowlatency -quality speed -rc vbr_peak" + GetRateArguments() +
                " -intra_refresh_mb " + RefreshBlocksPerFrame +
                " -color_range tv -colorspace bt709 -forced_idr 1 -f h264 -flush_packets 1 pipe:1";
      }

      // AMD wants the sweep as how many 16x16 blocks of the picture to redraw per frame
      private int RefreshBlocksPerFrame
      {
         get { return Math.Max(1, (outputWidth + 15) / 16 * ((outputHeight + 15) / 16) / RefreshCycleFrames); }
      }

      private string BuildCpuArguments()
      {
         return "-hide_banner -loglevel warning -init_hw_device d3d11va" + GetCpuCaptureChain() +
                " -c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p" +
                " -x264-params sliced-threads=0:slices=1:intra-refresh=1" +
                GetRateArguments() + " -f h264 -flush_packets 1 pipe:1";
      }

      private string BuildArguments(EncoderKind kind)
      {
         switch (kind)
         {
            case EncoderKind.Nvenc: return BuildNvencArguments();
            case EncoderKind.Amf: return BuildAmfArguments();
            case EncoderKind.QuickSync: return BuildQsvArguments();
            default: return BuildCpuArguments();
         }
      }

      // start on the encoder the user chose and, if it will not run, walk down the rest of the ones this
      // PC has. all of them failing is a fault worth stopping for, not worth retrying forever.
      private void RunPump(IPEndPoint target)
      {
         List<VideoEncoder> attempts = Server.EncodersToTry;
         bool anyEncoderWorked = false;

         foreach (VideoEncoder encoder in attempts)
         {
            if (!streaming) break;
            Server.Log("live: trying encoder " + encoder.Name);
            if (!PumpEncoder(BuildArguments(encoder.Kind), target)) continue;
            anyEncoderWorked = true;
            break;
         }

         streaming = false;
         if (anyEncoderWorked)
         {
            failedStarts = 0;
            Server.Log("live: stream to " + target + " ended");
            return;
         }

         if (++failedStarts >= FailedStartsBeforeGivingUp)
            Server.TripFuse("no encoder would start, " + failedStarts + " times running");
      }

      // spawns one ffmpeg and pumps its output to the client. returns false if the encoder
      // produced nothing (caller falls back to the next encoder), true otherwise.
      private bool PumpEncoder(string arguments, IPEndPoint target)
      {
         var startInfo = new ProcessStartInfo(ffmpegPath, arguments)
         {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
         };
         Process process;
         try { process = Process.Start(startInfo); }
         catch (Exception exception) { Server.Log("live: ffmpeg failed to start: " + exception.Message); return false; }
         ChildProcessJob.Assign(process);   // so a hard kill/crash of the server reaps ffmpeg too
         encoderProcess = process;

         // ffmpeg blocks if its error channel fills up, so drain it; keep the tail for diagnosis
         var errorTail = new StringBuilder();
         var errorThread = new Thread(() =>
         {
            string line;
            while ((line = process.StandardError.ReadLine()) != null)
            {
               if (errorTail.Length > 2000) errorTail.Length = 0;
               errorTail.AppendLine(line);
            }
         }) { IsBackground = true };
         errorThread.Start();

         var splitter = new LiveAnnexBSplitter();
         var chunk = new byte[64 * 1024];
         long frameId = 0;
         var firstFrameTimer = Stopwatch.StartNew();
         Stream output = process.StandardOutput.BaseStream;

         // output.Read blocks, so the first-frame timeout can't just be a check inside the loop - a silently
         // hung encoder would never reach it. instead kill the process after the timeout if no frame has come
         // out yet; that unblocks Read and the loop falls through to the next encoder.
         var firstFrameSeen = new ManualResetEventSlim(false);
         var timeoutThread = new Thread(() =>
         {
            if (!firstFrameSeen.Wait(FirstFrameTimeoutMs)) { try { if (!process.HasExited) process.Kill(); } catch { } }
         }) { IsBackground = true };
         timeoutThread.Start();

         while (streaming)
         {
            int read;
            try { read = output.Read(chunk, 0, chunk.Length); }
            catch (IOException) { break; }
            if (read <= 0) break;

            splitter.Push(chunk, read);
            LiveAnnexBSplitter.AccessUnit unit;
            while ((unit = splitter.TakeAccessUnit()) != null)
            {
               // stamp the moment the frame left the encoder; the PS3 measures every stage from here
               StreamSender.SendAccessUnit(socket, target, frameId++, unit.Data, 0, unit.Length, unit.Keyframe,
                                           StreamSender.NowUs, sendRateKbps);
               if (frameId == 1) { firstFrameSeen.Set(); Server.Log("live: first frame sent " + firstFrameTimer.ElapsedMilliseconds + "ms after encoder start"); }
            }
         }

         firstFrameSeen.Set();   // release the timeout thread if we're leaving for any other reason
         timeoutThread.Join();
         firstFrameSeen.Dispose();
         try { if (!process.HasExited) process.Kill(); } catch { }
         encoderProcess = null;
         if (frameId == 0)
         {
            Server.Log("live: encoder produced no frames. ffmpeg said:\n" + errorTail);
            return false;
         }
         Server.Log("live: sent " + frameId + " frames");
         return true;
      }
   }

   // incremental Annex-B access-unit splitter for a live pipe: push encoder bytes in, take
   // complete access units out. parameter units (SPS/PPS/SEI) attach to the picture that follows;
   // a picture unit closes the unit.
   internal sealed class LiveAnnexBSplitter
   {
      internal sealed class AccessUnit
      {
         public byte[] Data;
         public int Length;
         public bool Keyframe;
      }

      private byte[] pending = new byte[512 * 1024];
      private int pendingLength;
      private int scanPosition;
      private int unitStart = -1;
      private bool unitHasPicture, unitKeyframe;
      private AccessUnit completed;

      public void Push(byte[] data, int length)
      {
         if (pendingLength + length > pending.Length)
         {
            var grown = new byte[Math.Max(pending.Length * 2, pendingLength + length)];
            Array.Copy(pending, grown, pendingLength);
            pending = grown;
         }
         Array.Copy(data, 0, pending, pendingLength, length);
         pendingLength += length;
      }

      public AccessUnit TakeAccessUnit()
      {
         while (completed == null && scanPosition + 3 < pendingLength)
         {
            if (!(pending[scanPosition] == 0 && pending[scanPosition + 1] == 0 && pending[scanPosition + 2] == 1)) { scanPosition++; continue; }
            int nalStart = (scanPosition > 0 && pending[scanPosition - 1] == 0) ? scanPosition - 1 : scanPosition;
            int nalType = pending[scanPosition + 3] & 0x1F;
            bool isPicture = nalType == 1 || nalType == 5;

            if (unitStart >= 0 && unitHasPicture)
            {
               // a complete access unit ends where the next one's first unit begins
               completed = new AccessUnit { Data = new byte[nalStart - unitStart], Length = nalStart - unitStart, Keyframe = unitKeyframe };
               Array.Copy(pending, unitStart, completed.Data, 0, completed.Length);

               // drop the consumed bytes and restart the new unit at the front of the buffer
               Array.Copy(pending, nalStart, pending, 0, pendingLength - nalStart);
               pendingLength -= nalStart;
               scanPosition -= nalStart;
               unitStart = 0;
               unitHasPicture = false;
               unitKeyframe = false;
            }
            if (unitStart < 0) { unitStart = nalStart; unitHasPicture = false; unitKeyframe = false; }
            if (isPicture) { unitHasPicture = true; unitKeyframe |= nalType == 5; }
            scanPosition += 3;
         }
         AccessUnit result = completed;
         completed = null;
         return result;
      }
   }
}
