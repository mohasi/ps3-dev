using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Threading;

namespace CellStreamServer
{
   // fragments one H.264 access unit into UDP packets and sends them paced (used by LiveStreamer).
   //
   // fragment packet layout (20-byte header, big-endian, must match stream.c on the PS3):
   //   [0]='V' [1]='F' [2..5]=frameId [6..7]=fragIndex [8..9]=fragCount [10]=flags(bit0 keyframe)
   //   [11]=version [12..19]=encoder-exit time (server microseconds, for latency measurement)
   internal static class StreamSender
   {
      public const int FragmentPayloadBytes = 1300;
      public const int FragmentHeaderBytes = 20;
      public const int ProtocolVersion = 2;

      // one shared clock for the whole server: the PS3 syncs to it (TIME command) so it can measure how long
      // each frame took from encoder exit to appearing on screen.
      //
      // anchored to wall-clock time, not just to a stopwatch, so restarting the server does not rewind it. a
      // bare stopwatch restarts at zero, and a PS3 still streaming across a server restart then compared its
      // frames against a clock that had gone backwards - reporting a network latency of minus fourteen hours.
      // the stopwatch still provides the ticking (wall-clock time is coarse and can jump); it only sets zero.
      private static readonly Stopwatch Clock = Stopwatch.StartNew();
      private static readonly long StartUs = (DateTime.UtcNow - new DateTime(2020, 1, 1)).Ticks / 10;

      // Elapsed.Ticks is always 100ns units regardless of the timer's raw frequency, so /10 gives exact
      // microseconds. dividing ElapsedTicks by (Frequency/1000000) instead truncated the divisor and ran
      // the whole clock ~19% fast on any PC whose timer isn't a clean multiple of 1MHz (some VMs/chipsets).
      public static long NowUs { get { return StartUs + Clock.Elapsed.Ticks / 10; } }

      [ThreadStatic] private static byte[] packet;

      // pace by SEND RATE, not by a fixed slice of time. spreading every frame over the same
      // window fires a keyframe (3-4x a normal frame) as a huge burst - measured at ~400Mbps
      // instantaneous, well past what the link absorbs, so packets dropped once per keyframe and
      // each drop froze the picture. at a fixed rate a big frame simply takes proportionally
      // longer. busy-wait for sub-millisecond precision (Thread.Sleep can't do it).
      public static void SendAccessUnit(Socket socket, IPEndPoint target, long frameId, byte[] source, int offset, int length,
                                        bool keyframe, long captureUs, int sendRateKbps)
      {
         if (packet == null)
         {
            packet = new byte[FragmentHeaderBytes + FragmentPayloadBytes];
            packet[0] = (byte)'V';
            packet[1] = (byte)'F';
            packet[11] = ProtocolVersion;
         }

         int fragCount = (length + FragmentPayloadBytes - 1) / FragmentPayloadBytes;
         packet[2] = (byte)(frameId >> 24);
         packet[3] = (byte)(frameId >> 16);
         packet[4] = (byte)(frameId >> 8);
         packet[5] = (byte)frameId;
         packet[8] = (byte)(fragCount >> 8);
         packet[9] = (byte)fragCount;
         packet[10] = (byte)(keyframe ? 1 : 0);
         for (int i = 0; i < 8; i++) packet[12 + i] = (byte)(captureUs >> (56 - i * 8));

         // microseconds one fragment's worth of bits takes at the target rate
         long startUs = NowUs;
         long perFragmentUs = (FragmentHeaderBytes + FragmentPayloadBytes) * 8L * 1000L / Math.Max(1, sendRateKbps);
         for (int fragIndex = 0; fragIndex < fragCount; fragIndex++)
         {
            if (perFragmentUs > 0)
            {
               long dueUs = startUs + fragIndex * perFragmentUs;
               while (NowUs < dueUs) Thread.SpinWait(50);
            }
            int payloadBytes = Math.Min(FragmentPayloadBytes, length - fragIndex * FragmentPayloadBytes);
            packet[6] = (byte)(fragIndex >> 8);
            packet[7] = (byte)fragIndex;
            Array.Copy(source, offset + fragIndex * FragmentPayloadBytes, packet, FragmentHeaderBytes, payloadBytes);
            socket.SendTo(packet, FragmentHeaderBytes + payloadBytes, SocketFlags.None, target);
         }
      }
   }
}
