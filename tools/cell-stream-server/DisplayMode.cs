using System;
using System.Runtime.InteropServices;

namespace CellStreamServer
{
   // switches the PC's desktop to the resolution we stream at, and puts it back afterwards.
   //
   // this is the difference between sharp text and mush. shrinking a 1080p desktop into a 720p stream
   // is a 1.5x non-integer resize, and text does not survive it - no bitrate or encoder setting brought
   // it back (all tried). matching the desktop to the stream means no resize at all: every pixel of the
   // desktop is one pixel of video. it is also what Steam Link and Moonlight do, for the same reason.
   //
   // restoring is the part that must not fail, so it is defended three ways:
   //   - the mode is set as TEMPORARY, so Windows itself restores it if this process dies,
   //   - we restore explicitly when the stream stops,
   //   - and again on exit, on Ctrl+C, and on an unhandled crash.
   internal sealed class DisplayMode
   {
      private const int EnumCurrentSettings = -1;
      private const int CdsFullscreen = 0x00000004;   // "temporary": Windows undoes it when we exit
      private const int DispChangeSuccessful = 0;
      private const int DmPelsWidth = 0x00080000, DmPelsHeight = 0x00100000, DmDisplayFrequency = 0x00400000;

      private DevMode originalMode;
      private bool changed;

      public bool IsChanged { get { return changed; } }   // desktop is currently switched and owes a Restore

      public DisplayMode()
      {
         AppDomain.CurrentDomain.ProcessExit += (sender, args) => Restore();
         AppDomain.CurrentDomain.UnhandledException += (sender, args) => Restore();
         Console.CancelKeyPress += (sender, args) => Restore();
      }

      // returns true if the desktop is now at this size (or already was). false means we left it alone
      // and the stream will be scaled down as before - a worse picture, but nothing is broken.
      public bool MatchTo(int width, int height, int refreshHz)
      {
         if (changed) return true;

         var current = new DevMode { Size = (short)Marshal.SizeOf(typeof(DevMode)) };
         if (!EnumDisplaySettings(null, EnumCurrentSettings, ref current))
         {
            Server.Log("display: could not read the current resolution, streaming scaled instead");
            return false;
         }
         if (current.PelsWidth == width && current.PelsHeight == height) return true;   // already there

         originalMode = current;

         DevMode target = current;
         target.PelsWidth = width;
         target.PelsHeight = height;
         target.DisplayFrequency = refreshHz;
         target.Fields = DmPelsWidth | DmPelsHeight | DmDisplayFrequency;

         int result = ChangeDisplaySettingsEx(null, ref target, IntPtr.Zero, CdsFullscreen, IntPtr.Zero);
         if (result != DispChangeSuccessful)
         {
            // the monitor may not accept that size at that refresh rate; try letting Windows pick the rate
            target.Fields = DmPelsWidth | DmPelsHeight;
            result = ChangeDisplaySettingsEx(null, ref target, IntPtr.Zero, CdsFullscreen, IntPtr.Zero);
         }
         if (result != DispChangeSuccessful)
         {
            Server.Log("display: " + width + "x" + height + " was refused (code " + result + "), streaming scaled instead");
            return false;
         }

         changed = true;
         Server.Log("display: desktop switched to " + width + "x" + height + " (was " + current.PelsWidth + "x" +
                     current.PelsHeight + "); it will be restored when the stream stops");
         return true;
      }

      public void Restore()
      {
         if (!changed) return;
         changed = false;   // cleared first: a second call must never fight the first

         DevMode restoreTo = originalMode;
         restoreTo.Fields = DmPelsWidth | DmPelsHeight | DmDisplayFrequency;
         int result = ChangeDisplaySettingsEx(null, ref restoreTo, IntPtr.Zero, 0, IntPtr.Zero);
         if (result != DispChangeSuccessful)
            ChangeDisplaySettingsEx(null, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero);   // last resort: back to the saved default

         Server.Log("display: desktop restored to " + originalMode.PelsWidth + "x" + originalMode.PelsHeight);
      }

      [DllImport("user32.dll")]
      private static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DevMode devMode);

      [DllImport("user32.dll")]
      private static extern int ChangeDisplaySettingsEx(string deviceName, ref DevMode devMode, IntPtr window, int flags, IntPtr param);

      [DllImport("user32.dll")]
      private static extern int ChangeDisplaySettingsEx(string deviceName, IntPtr devMode, IntPtr window, int flags, IntPtr param);

      [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
      private struct DevMode
      {
         [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
         public short SpecVersion, DriverVersion, Size, DriverExtra;
         public int Fields;
         public short Orientation, PaperSize, PaperLength, PaperWidth, Scale, Copies, DefaultSource, PrintQuality;
         public short Color, Duplex, YResolution, TTOption, Collate;
         [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
         public short LogPixels;
         public int BitsPerPel, PelsWidth, PelsHeight, DisplayFlags, DisplayFrequency;
         public int ICMMethod, ICMIntent, MediaType, DitherType, Reserved1, Reserved2, PanningWidth, PanningHeight;
      }
   }
}
