using System;
using System.Diagnostics;
using System.Net;

namespace CellStreamServer
{
   // receives the PS3's controller and replays it on the PC, either as a virtual Xbox gamepad (for
   // games) or as the mouse and keyboard (for the desktop). the PS3 picks which with a PADMODE
   // message; the gamepad needs the ViGEmBus driver, and we fall back to the mouse without it.
   //
   // pad packet layout (20-byte header, big-endian, must match stream.c on the PS3):
   //   [0]='C' [1]='P' [2..5]=packetId [6..7]=buttons [8]=leftX [9]=leftY [10]=rightX [11]=rightY
   //   [12..19]=send time, already converted to OUR clock by the PS3
   internal sealed class PadReceiver
   {
      public const int PacketBytes = 20;
      private const int ReportIntervalMs = 2000;

      // bit positions must match the PadButton enum in the PS3's pad.h
      private static readonly string[] ButtonNames =
      {
         "up", "down", "left", "right", "cross", "circle", "square", "triangle",
         "L1", "R1", "L2", "R2", "start", "select", "L3", "R3"
      };

      private readonly DesktopInput desktopInput = new DesktopInput();
      private readonly VirtualGamepad gamepad = new VirtualGamepad();
      private readonly Stopwatch reportTimer = Stopwatch.StartNew();
      private long packetsReceived, packetsLost;
      private long intervalTripUs, intervalPackets;   // trip time is averaged over the last report window, not all time
      private long lastPacketId = -1;
      private int lastButtons;
      private bool gamepadMode, gamepadUnavailable;
      private string lastReportedState = "";

      // which PC device the pad drives. asking for the gamepad plugs a virtual one in and leaves it
      // there for the session; it comes back false when the driver is not installed.
      public void SetGamepadMode(bool wanted)
      {
         if (wanted == gamepadMode || (wanted && gamepadUnavailable)) return;
         if (wanted && !gamepad.TryOpen())
         {
            gamepadUnavailable = true;   // the PS3 asks once a second; do not keep trying
            Server.Log("pad: no virtual gamepad available. staying on mouse and keyboard.");
            return;
         }

         Release();   // let go of whatever the device we are leaving was holding down
         gamepadMode = wanted;
         Server.Log("pad: now driving " + (gamepadMode ? "a virtual Xbox gamepad" : "the mouse and keyboard"));
      }

      // a key typed on the PS3's on-screen keyboard, replayed on the PC keyboard
      public void TypeKey(char character)
      {
         desktopInput.TypeCharacter(character);
      }

      // the stream ended - let go of anything the PS3 was holding down, or it stays stuck on the PC
      public void Release()
      {
         desktopInput.ReleaseAll();
         gamepad.Send(0, 0, 0, 0, 0);
         lastButtons = 0;
         lastPacketId = -1;
      }

      public void Handle(byte[] packet, int length, IPEndPoint sender)
      {
         if (length < PacketBytes) return;

         long packetId = ReadInt32(packet, 2);
         int buttons = (packet[6] << 8) | packet[7];
         int leftX = (sbyte)packet[8], leftY = (sbyte)packet[9];
         int rightX = (sbyte)packet[10], rightY = (sbyte)packet[11];
         long sentUs = ReadInt64(packet, 12);

         if (lastPacketId >= 0 && packetId > lastPacketId + 1) packetsLost += packetId - lastPacketId - 1;
         lastPacketId = packetId;
         packetsReceived++;
         intervalTripUs += Math.Max(0, StreamSender.NowUs - sentUs);
         intervalPackets++;

         if (gamepadMode) gamepad.Send(buttons, leftX, leftY, rightX, rightY);
         else if (Server.SwapMouseSticks) desktopInput.Apply(buttons, rightX, rightY, leftX, leftY);
         else desktopInput.Apply(buttons, leftX, leftY, rightX, rightY);

         // log every press and release as it happens - that is what proves the channel end to end
         if (buttons != lastButtons)
         {
            string pressed = DescribeButtons(buttons & ~lastButtons);
            string released = DescribeButtons(lastButtons & ~buttons);
            if (pressed.Length > 0) Server.Log("pad: pressed " + pressed);
            if (released.Length > 0) Server.Log("pad: released " + released);
            lastButtons = buttons;
         }

         if (reportTimer.ElapsedMilliseconds >= ReportIntervalMs)
         {
            reportTimer.Restart();
            string state = "sticks L(" + leftX + "," + leftY + ") R(" + rightX + "," + rightY + ")";
            long tripMs = intervalPackets > 0 ? intervalTripUs / intervalPackets / 1000 : 0;
            intervalTripUs = 0;
            intervalPackets = 0;
            if (state != lastReportedState || packetsLost > 0)
            {
               lastReportedState = state;
               Server.Log("pad: " + state + ", " + packetsReceived + " packets, " + packetsLost + " lost, " +
                           tripMs + "ms PS3 to here");
            }
         }
      }

      private static string DescribeButtons(int mask)
      {
         string names = "";
         for (int bit = 0; bit < ButtonNames.Length; bit++)
            if ((mask & (1 << bit)) != 0) names += (names.Length > 0 ? "+" : "") + ButtonNames[bit];
         return names;
      }

      private static long ReadInt32(byte[] data, int offset)
      {
         return ((long)data[offset] << 24) | ((long)data[offset + 1] << 16) | ((long)data[offset + 2] << 8) | data[offset + 3];
      }

      private static long ReadInt64(byte[] data, int offset)
      {
         long value = 0;
         for (int i = 0; i < 8; i++) value = (value << 8) | data[offset + i];
         return value;
      }
   }
}
