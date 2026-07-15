using System;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace CellStreamServer
{
   // sends the captured speaker audio to the PS3 as small uncompressed packets. uncompressed on
   // purpose: at 48kHz stereo it costs ~1.5Mbps (a rounding error next to the video), and it skips
   // both an encoder on this side and a decoder on the PS3 - which is exactly the latency we are
   // trying not to spend. one packet is 5ms of sound, so a lost packet is a 5ms gap nobody hears.
   //
   // packet layout (16-byte header, big-endian, must match stream.c on the PS3):
   //   [0]='A' [1]='F' [2..5]=packetId [6..7]=frameCount [8..15]=capture time (server microseconds)
   //   payload: frameCount x (left, right) 16-bit signed samples
   internal sealed class AudioStreamer
   {
      public const int HeaderBytes = 16;
      private const int ChunkMs = 5;
      private const int MaxFramesPerPacket = 512;   // the PS3 (AUDIO_MAX_FRAMES) drops any packet larger than this
      private const int PrebufferMs = 20;          // ride out Windows' ~10ms delivery packets without gapping
      private const int PrebufferTimeoutMs = 500;  // ... but never wait longer than this for it (silence never fills it)

      private readonly Socket socket;
      private readonly AudioCapture capture = new AudioCapture();
      private volatile bool streaming;
      private Thread sendThread;
      private IPEndPoint currentTarget;

      public AudioStreamer(Socket socket)
      {
         this.socket = socket;
      }

      public void Start(IPEndPoint target)
      {
         if (streaming && target.Equals(currentTarget)) return;   // a repeated PLAY, not a new session
         Stop();
         if (!capture.Start()) return;   // no speakers to capture: video still streams
         streaming = true;
         currentTarget = target;
         sendThread = new Thread(() => RunSendLoop(target)) { IsBackground = true, Name = "audio-send" };
         sendThread.Start();
      }

      public void Stop()
      {
         streaming = false;
         if (sendThread != null) { sendThread.Join(500); sendThread = null; }
         capture.Stop();
      }

      private void RunSendLoop(IPEndPoint target)
      {
         int sampleRate = capture.SampleRate;
         // a high-rate device (176.4/192kHz) would put >512 frames in a 5ms chunk, which the PS3 drops
         // whole - losing all audio. cap the chunk at 512 frames; that just means shorter, more frequent
         // packets. pace by the chunk's real duration so the clock stays realtime whatever the cap does.
         int chunkFrames = Math.Min(sampleRate * ChunkMs / 1000, MaxFramesPerPacket);
         long chunkUs = (long)chunkFrames * 1000000 / sampleRate;
         var samples = new float[chunkFrames * 2];
         var packet = new byte[HeaderBytes + chunkFrames * 4];
         packet[0] = (byte)'A';
         packet[1] = (byte)'F';

         // tells the PS3 the rate so it can open its speaker feed. repeated once a second for the whole
         // stream: the PS3 may still be finishing the video handshake when the first ones arrive, and a
         // missed announcement would otherwise mean no sound for the entire session.
         byte[] info = Encoding.ASCII.GetBytes("AINFO " + sampleRate + " 2");
         int packetsPerSecond = Math.Max(1, (int)(1000000 / chunkUs));
         socket.SendTo(info, target);
         Server.Log("audio: streaming " + sampleRate + "Hz stereo to " + target + " (" + sampleRate * 32 / 1000 + "kbps)");

         // build a small cushion before playing, so Windows' ~10ms delivery bursts don't gap the sound. but
         // give up waiting quickly: Windows sends NOTHING while the PC is silent, so a session started with
         // nothing playing would otherwise wait here for ever and never send a single packet (it did).
         for (int waited = 0; streaming && waited < PrebufferTimeoutMs; waited++)
         {
            if (capture.BufferedFrames >= sampleRate * PrebufferMs / 1000) break;
            Thread.Sleep(1);
         }

         long startUs = StreamSender.NowUs;
         long packetId = 0, silentPackets = 0;
         try
         {
            while (streaming)
            {
               // packet N carries the sound due N chunks after we started
               long dueUs = startUs + packetId * chunkUs;
               long waitUs = dueUs - StreamSender.NowUs;
               if (waitUs > 2000) Thread.Sleep((int)(waitUs / 1000));
               while (StreamSender.NowUs < dueUs) Thread.SpinWait(50);

               if (capture.Read(samples, chunkFrames) == 0) silentPackets++;   // nothing playing: send silence, keep the clock going

               packet[2] = (byte)(packetId >> 24);
               packet[3] = (byte)(packetId >> 16);
               packet[4] = (byte)(packetId >> 8);
               packet[5] = (byte)packetId;
               packet[6] = (byte)(chunkFrames >> 8);
               packet[7] = (byte)chunkFrames;
               long captureUs = StreamSender.NowUs;
               for (int i = 0; i < 8; i++) packet[8 + i] = (byte)(captureUs >> (56 - i * 8));

               for (int i = 0; i < chunkFrames * 2; i++)
               {
                  float sample = samples[i];
                  if (sample > 1.0f) sample = 1.0f;
                  else if (sample < -1.0f) sample = -1.0f;
                  short value = (short)(sample * 32767.0f);
                  packet[HeaderBytes + i * 2] = (byte)(value >> 8);
                  packet[HeaderBytes + i * 2 + 1] = (byte)value;
               }
               socket.SendTo(packet, target);
               if (packetId % packetsPerSecond == 0) socket.SendTo(info, target);
               packetId++;
            }
         }
         catch (SocketException exception)
         {
            Server.Log("audio: send aborted: " + exception.Message);
         }
         Server.Log("audio: sent " + packetId + " packets (" + silentPackets + " silent, " + capture.DroppedFrames + " frames dropped)");
      }
   }
}
