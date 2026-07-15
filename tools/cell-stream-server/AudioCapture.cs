using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace CellStreamServer
{
   // captures whatever the PC is playing through its speakers (WASAPI loopback) and keeps the most
   // recent audio in a ring for the sender to drain. Windows exposes this only through COM, so the
   // interfaces below are declared by hand - that keeps the server a single .exe with no extra
   // libraries to ship and no audio driver for the user to install.
   //
   // Windows hands us the mix format it is already running at (in practice 48kHz stereo 32-bit float),
   // so there is no conversion or resampling here: we take its frames as they are and tell the PS3 the
   // rate, and the PS3's mixer resamples if it ever differs.
   internal sealed class AudioCapture
   {
      private const int ShareModeShared = 0;
      private const int StreamFlagsLoopback = 0x00020000;
      private const int ClsCtxAll = 0x17;
      private const int RenderDataFlow = 0, ConsoleRole = 0;
      private const int BufferFlagSilent = 0x2;
      private const int RequestedBufferDuration100Ns = 200000;   // 20ms of headroom in Windows' own buffer
      private const int PollIntervalMs = 2;

      private readonly object ringLock = new object();
      private float[] ring;                // interleaved stereo
      private int ringWritePosition, ringFrameCount;
      private volatile bool capturing;
      private Thread captureThread;

      public int SampleRate { get; private set; }
      public int DroppedFrames { get; private set; }   // ring overran: the sender wasn't draining fast enough

      // starts capture; returns false (with the reason logged) if Windows won't give us the speakers
      public bool Start()
      {
         Stop();
         IAudioClient audioClient;
         IAudioCaptureClient captureClient;
         int sampleRate, channels, bytesPerFrame;
         bool isFloat;
         if (!OpenLoopbackClient(out audioClient, out captureClient, out sampleRate, out channels, out bytesPerFrame, out isFloat))
            return false;

         SampleRate = sampleRate;
         lock (ringLock)
         {
            ring = new float[sampleRate * 2];   // one second of stereo
            ringWritePosition = ringFrameCount = 0;
         }
         DroppedFrames = 0;

         capturing = true;
         captureThread = new Thread(() => RunCapture(audioClient, captureClient, channels, bytesPerFrame, isFloat))
                         { IsBackground = true, Name = "audio-capture" };
         captureThread.Start();
         Server.Log("audio: capturing the speakers at " + sampleRate + "Hz, " + channels + " channels, " +
                     (isFloat ? "float" : "16-bit"));
         return true;
      }

      public void Stop()
      {
         capturing = false;
         if (captureThread != null) { captureThread.Join(500); captureThread = null; }
      }

      // copies `frames` stereo frames out of the ring. returns how many were real; the rest are left
      // silent. a short read means nothing was playing (Windows sends no packets during silence).
      public int Read(float[] destination, int frames)
      {
         lock (ringLock)
         {
            if (ring == null) return 0;
            int available = Math.Min(frames, ringFrameCount);
            int readPosition = ringWritePosition - ringFrameCount * 2;
            if (readPosition < 0) readPosition += ring.Length;
            for (int i = 0; i < available * 2; i++)
            {
               destination[i] = ring[readPosition];
               if (++readPosition == ring.Length) readPosition = 0;
            }
            for (int i = available * 2; i < frames * 2; i++) destination[i] = 0;
            ringFrameCount -= available;
            return available;
         }
      }

      public int BufferedFrames { get { lock (ringLock) { return ringFrameCount; } } }

      private void Write(float[] stereoFrames, int frames)
      {
         lock (ringLock)
         {
            int capacityFrames = ring.Length / 2;
            if (ringFrameCount + frames > capacityFrames)
            {
               DroppedFrames += ringFrameCount + frames - capacityFrames;
               ringFrameCount = capacityFrames - frames;
            }
            for (int i = 0; i < frames * 2; i++)
            {
               ring[ringWritePosition] = stereoFrames[i];
               if (++ringWritePosition == ring.Length) ringWritePosition = 0;
            }
            ringFrameCount += frames;
         }
      }

      private bool OpenLoopbackClient(out IAudioClient audioClient, out IAudioCaptureClient captureClient,
                                      out int sampleRate, out int channels, out int bytesPerFrame, out bool isFloat)
      {
         audioClient = null; captureClient = null;
         sampleRate = channels = bytesPerFrame = 0; isFloat = false;
         try
         {
            var enumerator = (IMMDeviceEnumerator)new MMDeviceEnumerator();
            IMMDevice speakers;
            enumerator.GetDefaultAudioEndpoint(RenderDataFlow, ConsoleRole, out speakers);

            object clientObject;
            var audioClientIid = new Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2");
            speakers.Activate(ref audioClientIid, ClsCtxAll, IntPtr.Zero, out clientObject);
            audioClient = (IAudioClient)clientObject;

            IntPtr formatPointer;
            audioClient.GetMixFormat(out formatPointer);
            var format = (WaveFormatEx)Marshal.PtrToStructure(formatPointer, typeof(WaveFormatEx));
            sampleRate = format.SamplesPerSecond;
            channels = format.Channels;
            bytesPerFrame = format.BlockAlign;
            isFloat = format.BitsPerSample == 32;   // WASAPI's mix format is float32 in practice; 16-bit is handled too
            if (format.BitsPerSample != 32 && format.BitsPerSample != 16)
            {
               Server.Log("audio: unsupported sample size (" + format.BitsPerSample + " bits), audio disabled");
               return false;
            }

            audioClient.Initialize(ShareModeShared, StreamFlagsLoopback, RequestedBufferDuration100Ns, 0, formatPointer, IntPtr.Zero);
            Marshal.FreeCoTaskMem(formatPointer);

            object serviceObject;
            var captureClientIid = new Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317");
            audioClient.GetService(ref captureClientIid, out serviceObject);
            captureClient = (IAudioCaptureClient)serviceObject;
            return true;
         }
         catch (Exception exception)
         {
            Server.Log("audio: could not open the speakers for capture, streaming video only (" + exception.Message + ")");
            return false;
         }
      }

      private void RunCapture(IAudioClient audioClient, IAudioCaptureClient captureClient, int channels, int bytesPerFrame, bool isFloat)
      {
         var stereo = new float[SampleRate * 2];   // worst case: one second in a single packet
         audioClient.Start();
         try
         {
            while (capturing)
            {
               int packetFrames;
               captureClient.GetNextPacketSize(out packetFrames);
               if (packetFrames == 0) { Thread.Sleep(PollIntervalMs); continue; }

               while (packetFrames > 0 && capturing)
               {
                  IntPtr data;
                  int frames, flags;
                  long devicePosition, counterPosition;
                  captureClient.GetBuffer(out data, out frames, out flags, out devicePosition, out counterPosition);
                  if (frames > 0)
                  {
                     if ((flags & BufferFlagSilent) != 0) Array.Clear(stereo, 0, frames * 2);
                     else ToStereoFloat(data, frames, channels, bytesPerFrame, isFloat, stereo);
                     Write(stereo, Math.Min(frames, stereo.Length / 2));
                  }
                  captureClient.ReleaseBuffer(frames);
                  captureClient.GetNextPacketSize(out packetFrames);
               }
            }
         }
         catch (Exception exception)
         {
            Server.Log("audio: capture stopped (" + exception.Message + ")");
         }
         try { audioClient.Stop(); } catch { }
      }

      // takes the first two channels of whatever Windows gives us (surround setups keep front left/right)
      private static void ToStereoFloat(IntPtr data, int frames, int channels, int bytesPerFrame, bool isFloat, float[] stereo)
      {
         var raw = new byte[frames * bytesPerFrame];
         Marshal.Copy(data, raw, 0, raw.Length);
         int sampleBytes = isFloat ? 4 : 2;
         for (int frame = 0; frame < frames; frame++)
         {
            int frameStart = frame * bytesPerFrame;
            for (int channel = 0; channel < 2; channel++)
            {
               int offset = frameStart + Math.Min(channel, channels - 1) * sampleBytes;
               stereo[frame * 2 + channel] = isFloat ? BitConverter.ToSingle(raw, offset)
                                                     : BitConverter.ToInt16(raw, offset) / 32768.0f;
            }
         }
      }

      [StructLayout(LayoutKind.Sequential, Pack = 2)]
      private struct WaveFormatEx
      {
         public short FormatTag, Channels;
         public int SamplesPerSecond, AverageBytesPerSecond;
         public short BlockAlign, BitsPerSample, ExtensionSize;
      }

      [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
      private class MMDeviceEnumerator { }

      [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
      private interface IMMDeviceEnumerator
      {
         int EnumAudioEndpoints(int dataFlow, int stateMask, out IntPtr devices);
         int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
      }

      [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
      private interface IMMDevice
      {
         int Activate(ref Guid interfaceId, int classContext, IntPtr activationParameters,
                      [MarshalAs(UnmanagedType.IUnknown)] out object instance);
      }

      [ComImport, Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
      private interface IAudioClient
      {
         int Initialize(int shareMode, int streamFlags, long bufferDuration, long periodicity, IntPtr format, IntPtr sessionId);
         int GetBufferSize(out int bufferFrames);
         int GetStreamLatency(out long latency);
         int GetCurrentPadding(out int paddingFrames);
         int IsFormatSupported(int shareMode, IntPtr format, out IntPtr closestMatch);
         int GetMixFormat(out IntPtr format);
         int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);
         int Start();
         int Stop();
         int Reset();
         int SetEventHandle(IntPtr handle);
         int GetService(ref Guid interfaceId, [MarshalAs(UnmanagedType.IUnknown)] out object instance);
      }

      [ComImport, Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
      private interface IAudioCaptureClient
      {
         int GetBuffer(out IntPtr data, out int frames, out int flags, out long devicePosition, out long counterPosition);
         int ReleaseBuffer(int frames);
         int GetNextPacketSize(out int frames);
      }
   }
}
