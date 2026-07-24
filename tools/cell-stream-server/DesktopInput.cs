using System;
using System.Runtime.InteropServices;

namespace CellStreamServer
{
   // drives the PC's mouse and keyboard from the PS3 pad, so the streamed desktop is usable from the
   // couch. this needs nothing installed: Windows lets any program synthesize keyboard and mouse input
   // (SendInput). it is NOT a gamepad - a game that wants a controller sees nothing here; that is what
   // VirtualGamepad is for, and SELECT + R3 on the PS3 swaps between the two.
   //
   // mapping (deliberately plain; change it here):
   //   left stick   mouse pointer          right stick scroll
   //   cross        left click             circle      right click
   //   square       enter                  triangle    backspace
   //   d-pad        arrow keys             L1 / R1     page up / page down
   //   select       escape
   internal sealed class DesktopInput
   {
      // the sticks rest slightly off centre once a pad has some age on it (the PS3's reads -6 at
      // rest), and without a dead zone that drifts the pointer across the screen on its own
      private const int StickDeadZone = 16;

      // pointer and scroll speeds are per SECOND, not per packet: the pad's send rate can vary, so moving
      // by elapsed time keeps the speed steady and the motion smooth however the packets are spaced. the
      // stick reads up to ~112 once the dead zone is off it. full tilt crosses the screen in ~1.8s; tune
      // these two numbers to taste.
      private const double StickFullTilt = 112.0;
      private const double PointerPixelsPerSecondAtFullTilt = 880.0;
      private const double ScrollNotchesPerSecondAtFullTilt = 42.0;

      private const double PointerSpeed = PointerPixelsPerSecondAtFullTilt / (StickFullTilt * StickFullTilt);
      private const double ScrollSpeed = ScrollNotchesPerSecondAtFullTilt / StickFullTilt;
      private const int WheelNotch = 120;   // one detent, as Windows counts them

      private const int InputMouse = 0, InputKeyboard = 1;
      private const uint MouseMove = 0x0001, MouseLeftDown = 0x0002, MouseLeftUp = 0x0004;
      private const uint MouseRightDown = 0x0008, MouseRightUp = 0x0010, MouseWheel = 0x0800;
      private const uint KeyUp = 0x0002, KeyUnicode = 0x0004;

      private const ushort VkReturn = 0x0D, VkBack = 0x08, VkEscape = 0x1B, VkTab = 0x09;
      private const ushort VkLeft = 0x25, VkUp = 0x26, VkRight = 0x27, VkDown = 0x28;
      private const ushort VkPrior = 0x21, VkNext = 0x22;

      // one row per pad button we forward, in PadButton bit order where it matters
      private struct KeyBinding
      {
         public int Bit;
         public ushort Key;
         public KeyBinding(int bit, ushort key) { Bit = bit; Key = key; }
      }

      private static readonly KeyBinding[] KeyBindings =
      {
         new KeyBinding(PadBits.Up, VkUp), new KeyBinding(PadBits.Down, VkDown),
         new KeyBinding(PadBits.Left, VkLeft), new KeyBinding(PadBits.Right, VkRight),
         new KeyBinding(PadBits.Square, VkReturn), new KeyBinding(PadBits.Triangle, VkBack),
         new KeyBinding(PadBits.L1, VkPrior), new KeyBinding(PadBits.R1, VkNext),
         new KeyBinding(PadBits.Select, VkEscape)
      };

      private int lastButtons;
      private double pointerCarryX, pointerCarryY, scrollCarry;   // sub-pixel remainders, so slow moves still move
      private readonly System.Diagnostics.Stopwatch clock = System.Diagnostics.Stopwatch.StartNew();
      private double lastApplySeconds;

      public void Apply(int buttons, int leftX, int leftY, int rightX, int rightY)
      {
         // seconds since the previous packet, capped so a gap (or the first packet) can't lurch the pointer
         double now = clock.Elapsed.TotalSeconds;
         double elapsedSeconds = now - lastApplySeconds;
         lastApplySeconds = now;
         if (elapsedSeconds < 0) elapsedSeconds = 0;
         if (elapsedSeconds > 0.05) elapsedSeconds = 0.05;

         MovePointer(leftX, leftY, elapsedSeconds);
         Scroll(rightY, elapsedSeconds);

         int pressed = buttons & ~lastButtons, released = lastButtons & ~buttons;

         if ((pressed & (1 << PadBits.Cross)) != 0) SendMouse(MouseLeftDown, 0, 0, 0);
         if ((released & (1 << PadBits.Cross)) != 0) SendMouse(MouseLeftUp, 0, 0, 0);
         if ((pressed & (1 << PadBits.Circle)) != 0) SendMouse(MouseRightDown, 0, 0, 0);
         if ((released & (1 << PadBits.Circle)) != 0) SendMouse(MouseRightUp, 0, 0, 0);

         foreach (KeyBinding binding in KeyBindings)
         {
            if ((pressed & (1 << binding.Bit)) != 0) SendKey(binding.Key, false);
            if ((released & (1 << binding.Bit)) != 0) SendKey(binding.Key, true);
         }
         lastButtons = buttons;
      }

      // releases everything still held, so nothing is left stuck down when the stream ends
      public void ReleaseAll()
      {
         Apply(0, 0, 0, 0, 0);
      }

      // types one character from the PS3's on-screen keyboard. control keys map to real keys;
      // every other character is injected as Unicode, so the PC keyboard layout never matters.
      public void TypeCharacter(char character)
      {
         switch (character)
         {
            case '\b': SendKey(VkBack, false); SendKey(VkBack, true); break;
            case '\t': SendKey(VkTab, false); SendKey(VkTab, true); break;
            case '\n': SendKey(VkReturn, false); SendKey(VkReturn, true); break;
            default: SendUnicode(character); break;
         }
      }

      private static void SendUnicode(char character)
      {
         var down = new Input { Type = InputKeyboard };
         down.Union.Keyboard = new KeyboardInput { ScanCode = character, Flags = KeyUnicode };
         var up = new Input { Type = InputKeyboard };
         up.Union.Keyboard = new KeyboardInput { ScanCode = character, Flags = KeyUnicode | KeyUp };
         SendInput(2, new[] { down, up }, Marshal.SizeOf(typeof(Input)));
      }

      // squared response: small stick movements stay slow and precise, big ones move fast. a linear
      // pointer is either too slow to cross the screen or too twitchy to hit anything.
      private void MovePointer(int stickX, int stickY, double elapsedSeconds)
      {
         double x = ApplyDeadZone(stickX), y = ApplyDeadZone(stickY);
         if (x == 0 && y == 0) return;

         pointerCarryX += x * Math.Abs(x) * PointerSpeed * elapsedSeconds;
         pointerCarryY += y * Math.Abs(y) * PointerSpeed * elapsedSeconds;
         int moveX = (int)pointerCarryX, moveY = (int)pointerCarryY;
         pointerCarryX -= moveX;
         pointerCarryY -= moveY;
         if (moveX != 0 || moveY != 0) SendMouse(MouseMove, moveX, moveY, 0);
      }

      private void Scroll(int stickY, double elapsedSeconds)
      {
         double y = ApplyDeadZone(stickY);
         if (y == 0) { scrollCarry = 0; return; }

         scrollCarry -= y * ScrollSpeed * elapsedSeconds;   // stick down (positive) scrolls the page down
         int notches = (int)scrollCarry;
         if (notches == 0) return;
         scrollCarry -= notches;
         SendMouse(MouseWheel, 0, 0, notches * WheelNotch);
      }

      private static double ApplyDeadZone(int value)
      {
         if (value > -StickDeadZone && value < StickDeadZone) return 0;
         return value > 0 ? value - StickDeadZone : value + StickDeadZone;
      }

      private static void SendMouse(uint flags, int dx, int dy, int wheelData)
      {
         var input = new Input { Type = InputMouse };
         input.Union.Mouse = new MouseInput { X = dx, Y = dy, Data = wheelData, Flags = flags };
         SendOne(input);
      }

      private static void SendKey(ushort key, bool release)
      {
         var input = new Input { Type = InputKeyboard };
         input.Union.Keyboard = new KeyboardInput { VirtualKey = key, Flags = release ? KeyUp : 0 };
         SendOne(input);
      }

      private static void SendOne(Input input)
      {
         var batch = new[] { input };
         SendInput(1, batch, Marshal.SizeOf(typeof(Input)));
      }

      [DllImport("user32.dll", SetLastError = true)]
      private static extern uint SendInput(uint count, Input[] inputs, int size);

      [StructLayout(LayoutKind.Sequential)]
      private struct Input
      {
         public int Type;
         public InputUnion Union;
      }

      [StructLayout(LayoutKind.Explicit)]
      private struct InputUnion
      {
         [FieldOffset(0)] public MouseInput Mouse;
         [FieldOffset(0)] public KeyboardInput Keyboard;
      }

      [StructLayout(LayoutKind.Sequential)]
      private struct MouseInput
      {
         public int X, Y;
         public int Data;
         public uint Flags;
         public uint Time;
         public IntPtr ExtraInfo;
      }

      [StructLayout(LayoutKind.Sequential)]
      private struct KeyboardInput
      {
         public ushort VirtualKey, ScanCode;
         public uint Flags;
         public uint Time;
         public IntPtr ExtraInfo;
      }
   }

   // bit positions of the PS3 pad, matching the PadButton enum in the PS3's pad.h
   internal static class PadBits
   {
      public const int Up = 0, Down = 1, Left = 2, Right = 3;
      public const int Cross = 4, Circle = 5, Square = 6, Triangle = 7;
      public const int L1 = 8, R1 = 9, L2 = 10, R2 = 11;
      public const int Start = 12, Select = 13, L3 = 14, R3 = 15;
   }
}
