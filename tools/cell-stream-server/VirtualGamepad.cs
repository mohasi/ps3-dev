using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace CellStreamServer
{
   // a virtual Xbox 360 controller. games and Windows itself see a real gamepad plugged in.
   //
   // this talks to the ViGEmBus driver directly. ViGEm normally ships a client DLL, but that DLL is a
   // thin wrapper around three driver calls (plug in, send a report, unplug), so we make them ourselves
   // and there is nothing to ship next to the driver.
   //
   // the driver IS required - Windows has no way for a program to fake a gamepad without one.
   // TryOpen() returns false when it is not installed, and the caller falls back to mouse + keyboard.
   internal sealed class VirtualGamepad : IDisposable
   {
      // the ViGEmBus driver's device interface, and its three calls
      private static readonly Guid BusInterface = new Guid("96E42B22-F5E9-42F8-B043-ED0F932F014F");
      private const uint IoctlPlugIn = 0x002AA004;
      private const uint IoctlUnplug = 0x002AA008;
      private const uint IoctlSubmitReport = 0x002AA808;
      private const int Xbox360Wired = 0;
      private const int MaxSerial = 16;          // the driver's per-bus device limit
      private const int PlugInSettleMs = 250;    // let Windows finish enumerating the new gamepad
      private const string SetupFileName = "ViGEmBusSetup.exe";
      private const int InstallTimeoutMs = 180000;
      private const int DriverSettleMs = 2000;

      // XINPUT_GAMEPAD button bits, as every Windows game reads them
      private const ushort PadUp = 0x0001, PadDown = 0x0002, PadLeft = 0x0004, PadRight = 0x0008;
      private const ushort PadStart = 0x0010, PadBack = 0x0020, PadLeftThumb = 0x0040, PadRightThumb = 0x0080;
      private const ushort PadLeftShoulder = 0x0100, PadRightShoulder = 0x0200;
      private const ushort PadA = 0x1000, PadB = 0x2000, PadX = 0x4000, PadY = 0x8000;

      private const int StickDeadZone = 12;      // the PS3's sticks rest a few counts off centre
      private const double StickFullTilt = 115.0;

      private static bool installTried;
      private IntPtr busHandle = InvalidHandle;
      private uint serial;

      public bool IsOpen { get { return busHandle != InvalidHandle; } }

      // false means we have no gamepad: the driver is missing and installing it did not work
      public bool TryOpen()
      {
         if (IsOpen) return true;

         string devicePath = FindBusDevicePath();
         if (devicePath == null && TryInstallDriver()) devicePath = FindBusDevicePath();
         if (devicePath == null) return false;

         IntPtr handle = CreateFile(devicePath, GenericRead | GenericWrite, FileShareRead | FileShareWrite,
                                    IntPtr.Zero, OpenExisting, FileAttributeNormal, IntPtr.Zero);
         if (handle == InvalidHandle) return false;

         // the bus can hold several gamepads; take the first free slot
         for (uint candidate = 1; candidate <= MaxSerial; candidate++)
         {
            var plugIn = new PlugInTarget
            {
               Size = (uint)Marshal.SizeOf(typeof(PlugInTarget)),
               SerialNo = candidate,
               TargetType = Xbox360Wired
            };
            if (!Call(handle, IoctlPlugIn, plugIn)) continue;

            busHandle = handle;
            serial = candidate;
            Thread.Sleep(PlugInSettleMs);
            Send(0, 0, 0, 0, 0);   // a first report at rest, so nothing reads as held
            return true;
         }

         CloseHandle(handle);
         return false;
      }

      // buttons is the PS3's bitmask (see PadBits); sticks are -128..127 with y positive downwards
      public void Send(int buttons, int leftX, int leftY, int rightX, int rightY)
      {
         if (!IsOpen) return;

         ushort xboxButtons = ToXboxButtons(buttons);

         var submit = new SubmitReport
         {
            Size = (uint)Marshal.SizeOf(typeof(SubmitReport)),
            SerialNo = serial,
            Buttons = xboxButtons,
            LeftTrigger = (buttons & (1 << PadBits.L2)) != 0 ? (byte)255 : (byte)0,
            RightTrigger = (buttons & (1 << PadBits.R2)) != 0 ? (byte)255 : (byte)0,
            ThumbLX = ToXboxAxis(leftX),
            ThumbLY = ToXboxAxis(-leftY),     // the pad reads y downwards, XInput reads it upwards
            ThumbRX = ToXboxAxis(rightX),
            ThumbRY = ToXboxAxis(-rightY)
         };
         Call(busHandle, IoctlSubmitReport, submit);
      }

      public void Dispose()
      {
         if (!IsOpen) return;

         Call(busHandle, IoctlUnplug, new UnplugTarget { Size = (uint)Marshal.SizeOf(typeof(UnplugTarget)), SerialNo = serial });
         CloseHandle(busHandle);
         busHandle = InvalidHandle;
      }

      // the driver's installer ships next to us. installing it needs administrator rights, so Windows
      // asks for permission once, on the PC, the first time a gamepad is wanted - and never again.
      private static bool TryInstallDriver()
      {
         if (installTried) return false;
         installTried = true;

         string setup = Path.Combine(Server.ExeFolder, SetupFileName);
         if (!File.Exists(setup))
         {
            Server.Log("pad: " + SetupFileName + " is not next to the server, so the gamepad driver cannot be installed");
            return false;
         }

         Server.Log("pad: installing the ViGEmBus gamepad driver - Windows will ask for permission on the PC ...");
         try
         {
            var start = new ProcessStartInfo(setup, "/quiet /norestart") { UseShellExecute = true, Verb = "runas" };
            Process installer = Process.Start(start);
            installer.WaitForExit(InstallTimeoutMs);
            Server.Log("pad: driver installer finished (exit code " + installer.ExitCode + ")");
            Thread.Sleep(DriverSettleMs);   // Windows takes a moment to bring the new bus up
            return installer.ExitCode == 0;
         }
         catch (Exception exception)
         {
            Server.Log("pad: could not install the gamepad driver: " + exception.Message);
            return false;
         }
      }

      private static ushort ToXboxButtons(int buttons)
      {
         ushort result = 0;
         if ((buttons & (1 << PadBits.Up)) != 0) result |= PadUp;
         if ((buttons & (1 << PadBits.Down)) != 0) result |= PadDown;
         if ((buttons & (1 << PadBits.Left)) != 0) result |= PadLeft;
         if ((buttons & (1 << PadBits.Right)) != 0) result |= PadRight;
         if ((buttons & (1 << PadBits.Cross)) != 0) result |= PadA;
         if ((buttons & (1 << PadBits.Circle)) != 0) result |= PadB;
         if ((buttons & (1 << PadBits.Square)) != 0) result |= PadX;
         if ((buttons & (1 << PadBits.Triangle)) != 0) result |= PadY;
         if ((buttons & (1 << PadBits.L1)) != 0) result |= PadLeftShoulder;
         if ((buttons & (1 << PadBits.R1)) != 0) result |= PadRightShoulder;
         if ((buttons & (1 << PadBits.Start)) != 0) result |= PadStart;
         if ((buttons & (1 << PadBits.Select)) != 0) result |= PadBack;
         if ((buttons & (1 << PadBits.L3)) != 0) result |= PadLeftThumb;
         if ((buttons & (1 << PadBits.R3)) != 0) result |= PadRightThumb;
         return result;
      }

      // the PS3 reads -128..127 and rests a little off centre; XInput wants -32768..32767 centred
      private static short ToXboxAxis(int value)
      {
         double tilt = 0;
         if (value >= StickDeadZone) tilt = (value - StickDeadZone) / (StickFullTilt - StickDeadZone);
         else if (value <= -StickDeadZone) tilt = (value + StickDeadZone) / (StickFullTilt - StickDeadZone);

         if (tilt > 1) tilt = 1;
         if (tilt < -1) tilt = -1;
         return (short)(tilt * 32767);
      }

      private static bool Call(IntPtr handle, uint ioctl, object request)
      {
         int size = Marshal.SizeOf(request);
         IntPtr buffer = Marshal.AllocHGlobal(size);
         try
         {
            Marshal.StructureToPtr(request, buffer, false);
            int written;
            return DeviceIoControl(handle, ioctl, buffer, size, IntPtr.Zero, 0, out written, IntPtr.Zero);
         }
         finally
         {
            Marshal.FreeHGlobal(buffer);
         }
      }

      // asks Windows where the ViGEmBus driver's device lives; null when it is not installed
      private static string FindBusDevicePath()
      {
         Guid interfaceGuid = BusInterface;
         IntPtr deviceSet = SetupDiGetClassDevs(ref interfaceGuid, IntPtr.Zero, IntPtr.Zero, DigcfPresent | DigcfDeviceInterface);
         if (deviceSet == InvalidHandle) return null;

         try
         {
            var deviceInterface = new DeviceInterfaceData();
            deviceInterface.Size = Marshal.SizeOf(typeof(DeviceInterfaceData));
            if (!SetupDiEnumDeviceInterfaces(deviceSet, IntPtr.Zero, ref interfaceGuid, 0, ref deviceInterface)) return null;

            int required = 0;
            SetupDiGetDeviceInterfaceDetail(deviceSet, ref deviceInterface, IntPtr.Zero, 0, ref required, IntPtr.Zero);
            if (required == 0) return null;

            IntPtr detail = Marshal.AllocHGlobal(required);
            try
            {
               // the detail block is a size field followed by the path; 64-bit pads the field out to 8
               Marshal.WriteInt32(detail, IntPtr.Size == 8 ? 8 : 4 + Marshal.SystemDefaultCharSize);
               if (!SetupDiGetDeviceInterfaceDetail(deviceSet, ref deviceInterface, detail, required, ref required, IntPtr.Zero))
                  return null;
               return Marshal.PtrToStringAuto(new IntPtr(detail.ToInt64() + 4));
            }
            finally
            {
               Marshal.FreeHGlobal(detail);
            }
         }
         finally
         {
            SetupDiDestroyDeviceInfoList(deviceSet);
         }
      }

      [StructLayout(LayoutKind.Sequential)]
      private struct PlugInTarget
      {
         public uint Size, SerialNo;
         public int TargetType;
         public ushort VendorId, ProductId;
      }

      [StructLayout(LayoutKind.Sequential)]
      private struct UnplugTarget
      {
         public uint Size, SerialNo;
      }

      // ULONG size, ULONG serial, then the XUSB report Windows itself hands to games
      [StructLayout(LayoutKind.Sequential)]
      private struct SubmitReport
      {
         public uint Size, SerialNo;
         public ushort Buttons;
         public byte LeftTrigger, RightTrigger;
         public short ThumbLX, ThumbLY, ThumbRX, ThumbRY;
      }

      [StructLayout(LayoutKind.Sequential)]
      private struct DeviceInterfaceData
      {
         public int Size;
         public Guid InterfaceClassGuid;
         public int Flags;
         public IntPtr Reserved;
      }

      private static readonly IntPtr InvalidHandle = new IntPtr(-1);
      private const uint GenericRead = 0x80000000, GenericWrite = 0x40000000;
      private const uint FileShareRead = 1, FileShareWrite = 2;
      private const uint OpenExisting = 3, FileAttributeNormal = 0x80;
      private const int DigcfPresent = 0x02, DigcfDeviceInterface = 0x10;

      [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
      private static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr security, uint disposition,
                                              uint flags, IntPtr template);

      [DllImport("kernel32.dll", SetLastError = true)]
      private static extern bool DeviceIoControl(IntPtr device, uint ioctl, IntPtr input, int inputSize,
                                                 IntPtr output, int outputSize, out int written, IntPtr overlapped);

      [DllImport("kernel32.dll", SetLastError = true)]
      private static extern bool CloseHandle(IntPtr handle);

      [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Auto)]
      private static extern IntPtr SetupDiGetClassDevs(ref Guid interfaceGuid, IntPtr enumerator, IntPtr window, int flags);

      [DllImport("setupapi.dll", SetLastError = true)]
      private static extern bool SetupDiEnumDeviceInterfaces(IntPtr deviceSet, IntPtr deviceInfo, ref Guid interfaceGuid,
                                                             int index, ref DeviceInterfaceData deviceInterface);

      [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Auto)]
      private static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr deviceSet, ref DeviceInterfaceData deviceInterface,
                                                                 IntPtr detail, int detailSize, ref int required,
                                                                 IntPtr deviceInfoData);

      [DllImport("setupapi.dll", SetLastError = true)]
      private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceSet);
   }
}
