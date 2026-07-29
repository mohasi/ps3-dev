using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32;


namespace CellStreamServer
{
   // the cell-stream server: captures the desktop, sends it to the PS3, and replays the PS3's pad here.
   // one UDP socket on :38310:
   //  - broadcasts a discovery beacon to :38311 every second so the PS3 finds us
   internal static class Server
   {
      private const int ServerPort = 38310;
      private const int BeaconPort = 38311;
      private const int ClientTimeoutMs = 3000;   // no word from the PS3 for this long = it is gone
      // before the first pad arrives the PS3 is still finishing the handshake (and its resolution switch), so
      // give the stream a longer grace to latch on - tearing down at ClientTimeoutMs mid-handshake made it loop.
      private const int StreamStartupGraceMs = 10000;
      private const int WatchdogTickMs = 500;
      private static volatile bool streamConfirmed;   // a pad packet has arrived, so the PS3 really is streaming
      private const int BindAttempts = 25, BindRetryMs = 200;   // ~5s for the copy we are replacing to let the port go

      private static readonly Stopwatch SinceLastClientPacket = Stopwatch.StartNew();

      private static Socket socket;
      private static LiveStreamer liveStreamer;
      private static AudioStreamer audioStreamer;
      private static readonly PadReceiver padReceiver = new PadReceiver();
      private static readonly DisplayMode displayMode = new DisplayMode();
      private static readonly object streamLock = new object();   // serialises start against stop across threads

      private const string SettingsKey = @"Software\CellStreamServer";
      // user preference: in mouse mode, drive the pointer from the right stick instead of the left
      public static volatile bool SwapMouseSticks;

      [DllImport("winmm.dll")]
      private static extern uint timeBeginPeriod(uint milliseconds);

      [DllImport("kernel32.dll")]
      private static extern uint SetThreadExecutionState(uint flags);

      private const uint EsContinuous = 0x80000000, EsDisplayRequired = 0x00000002, EsSystemRequired = 0x00000001;

      // capturing a slept display returns black frames, so hold the display on and the PC awake WHILE
      // streaming. when idle we release it (EsContinuous alone) so the screen sleeps normally.
      private static void keepDisplayAwake(bool streaming)
      {
         SetThreadExecutionState(streaming ? (EsContinuous | EsDisplayRequired | EsSystemRequired) : EsContinuous);
      }

      private static void loadPreferences()
      {
         try
         {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(SettingsKey))
               SwapMouseSticks = key != null && (key.GetValue("SwapMouseSticks") as string) == "1";
         }
         catch { }
      }

      public static void SetSwapMouseSticks(bool on)
      {
         SwapMouseSticks = on;
         try
         {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(SettingsKey))
               if (key != null) key.SetValue("SwapMouseSticks", on ? "1" : "0");
         }
         catch { }
      }

      // the settings, deliberately baked in: this is an appliance, not a command line. (the PS3 will get
      // to choose them over the wire later, which is why they are not a config file yet.)
      private const int Fps = 60;
      private const int Kbps = 10000;
      private const int Width = 1280, Height = 720;
      private const int SendRateKbps = Kbps * 3;   // packets may leave faster than the video's own rate

      // ffmpeg.exe and ViGEmBusSetup.exe ship next to us
      public static string ExeFolder
      {
         get { return Path.GetDirectoryName(System.Reflection.Assembly.GetExecutingAssembly().Location); }
      }

      // brings the server up on its own threads. the window is only a view onto it: closing the window
      // leaves all of this running, which is the whole point of living in the tray.
      public static bool Start()
      {
         timeBeginPeriod(1);   // 1ms Thread.Sleep resolution; the video pacing depends on it
         loadPreferences();
         if (!BindSocket()) return false;

         string ffmpegPath = Path.Combine(ExeFolder, "ffmpeg.exe");
         bool ffmpegBundled = File.Exists(ffmpegPath);   // the shipped folder carries ffmpeg.exe beside the server
         if (!ffmpegBundled) ffmpegPath = "ffmpeg";       // dev fallback: use one on PATH
         liveStreamer = new LiveStreamer(socket, ffmpegPath, Fps, Kbps, Width, Height, SendRateKbps);
         audioStreamer = new AudioStreamer(socket);   // desktop sound goes with the desktop picture
         IsArmed = true;   // the server runs by default; only a fault or the user stops it

         AvailableEncoders = VideoEncoders.DetectAvailable(ffmpegPath);
         chosenEncoder = VideoEncoders.LoadChoice(AvailableEncoders);
         if (chosenEncoder == null)
            TripFuse(ffmpegBundled ? "this PC has no working video encoder" : "ffmpeg.exe is missing from the server folder");
         Log("ready: " + SettingsSummary + ", ffmpeg = " + ffmpegPath);

         new Thread(RunBeaconLoop) { IsBackground = true, Name = "beacon" }.Start();
         new Thread(RunClientWatchdog) { IsBackground = true, Name = "watchdog" }.Start();
         new Thread(RunReceiveLoop) { IsBackground = true, Name = "receive" }.Start();
         return true;
      }

      public static string SettingsSummary
      {
         get { return Width + "x" + Height + " at " + Fps + "fps, " + Kbps / 1000 + " Mbps, intra refresh"; }
      }

      // when a new build replaces us, the copy we are replacing is still letting go of the port for a
      // moment. so wait for it rather than falling over on the way up.
      private static bool BindSocket()
      {
         for (int attempt = 0; attempt < BindAttempts; attempt++)
         {
            try
            {
               socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
               socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.Broadcast, true);
               socket.SendBufferSize = 1024 * 1024;
               socket.Bind(new IPEndPoint(IPAddress.Any, ServerPort));
               Log("listening on udp :" + ServerPort + ", beaconing to :" + BeaconPort);
               return true;
            }
            catch (SocketException)
            {
               socket.Close();
               Thread.Sleep(BindRetryMs);
            }
         }
         Log("could not listen on udp :" + ServerPort + " - another copy of the server is still holding it. giving up.");
         return false;
      }

      // what the tray shows
      public static bool IsPs3Connected { get { return liveStreamer != null && liveStreamer.IsStreaming; } }
      public static string ConnectedPs3 { get; private set; }

      // the fuse. armed, the server answers the PS3; tripped, it ignores it and leaves the desktop alone.
      // it trips itself when the encoder will not start, because retrying that forever flapped the
      // desktop resolution on and off and made the PC unusable. only the user re-arms it.
      public static bool IsArmed { get; private set; }
      public static string TripReason { get; private set; }

      public static void Arm()
      {
         if (IsArmed) return;
         IsArmed = true;
         TripReason = null;
         liveStreamer.ResetFailures();
         Log("started: waiting for the PS3");
      }

      public static void Disarm(string why)
      {
         if (!IsArmed) return;
         IsArmed = false;
         TripReason = why;
         StopStreaming(why);
         Log("stopped: " + why);
      }

      public static void TripFuse(string fault)
      {
         Disarm(fault + ". press Start once it is fixed.");
      }

      // the encoders this PC can actually run, best first, and the one to use
      public static List<VideoEncoder> AvailableEncoders { get; private set; }
      private static VideoEncoder chosenEncoder;

      public static VideoEncoder ChosenEncoder
      {
         get { return chosenEncoder; }
         set
         {
            if (value == null || value == chosenEncoder) return;
            chosenEncoder = value;
            VideoEncoders.SaveChoice(value);
            Log("encoders: using " + value.Name + " from now on");
         }
      }

      // the chosen one first, then the rest as fallbacks
      public static List<VideoEncoder> EncodersToTry
      {
         get
         {
            var order = new List<VideoEncoder>();
            if (chosenEncoder != null) order.Add(chosenEncoder);
            foreach (VideoEncoder encoder in AvailableEncoders)
               if (encoder != chosenEncoder) order.Add(encoder);
            return order;
         }
      }

      // the tray's Quit: put the desktop back before we go, or it is left at the streaming resolution
      public static void Shutdown()
      {
         StopStreaming("the server is shutting down");
      }

      // the machine can have several network adapters (VirtualBox adds virtual ones), and a plain
      // 255.255.255.255 broadcast only leaves through ONE of them - often the wrong one. so beacon
      // to every adapter's own broadcast address (e.g. 10.0.0.255) plus the global one.
      private static List<IPEndPoint> GetBeaconTargets()
      {
         var targets = new List<IPEndPoint> { new IPEndPoint(IPAddress.Broadcast, BeaconPort) };
         foreach (NetworkInterface adapter in NetworkInterface.GetAllNetworkInterfaces())
         {
            if (adapter.OperationalStatus != OperationalStatus.Up) continue;
            if (adapter.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
            foreach (UnicastIPAddressInformation address in adapter.GetIPProperties().UnicastAddresses)
            {
               if (address.Address.AddressFamily != AddressFamily.InterNetwork || address.IPv4Mask == null) continue;
               byte[] ip = address.Address.GetAddressBytes();
               byte[] mask = address.IPv4Mask.GetAddressBytes();
               var broadcastBytes = new byte[4];
               for (int i = 0; i < 4; i++) broadcastBytes[i] = (byte)(ip[i] | ~mask[i]);
               var target = new IPEndPoint(new IPAddress(broadcastBytes), BeaconPort);
               if (!targets.Contains(target)) targets.Add(target);
            }
         }
         return targets;
      }

      private static void RunBeaconLoop()
      {
         byte[] beacon = Encoding.ASCII.GetBytes("CELLSTREAM 1");
         List<IPEndPoint> targets = GetBeaconTargets();
         var described = new StringBuilder();
         foreach (IPEndPoint target in targets) described.Append(target.Address).Append(" ");
         Log("beaconing to: " + described);

         int secondsSinceRefresh = 0;
         while (true)
         {
            foreach (IPEndPoint target in targets)
            {
               try { socket.SendTo(beacon, target); }
               catch (SocketException exception) { Log("beacon to " + target.Address + " failed: " + exception.Message); }
            }
            Thread.Sleep(1000);
            if (++secondsSinceRefresh >= 30) { targets = GetBeaconTargets(); secondsSinceRefresh = 0; }   // pick up NIC changes
         }
      }

      private static void RunReceiveLoop()
      {
         var buffer = new byte[2048];
         EndPoint sender = new IPEndPoint(IPAddress.Any, 0);
         while (true)
         {
            int length;
            try { length = socket.ReceiveFrom(buffer, ref sender); }
            catch (SocketException) { continue; }   // ICMP port-unreachable from a previous send surfaces here
            if (length <= 0) continue;
            SinceLastClientPacket.Restart();   // proof the PS3 is still there (see RunClientWatchdog)

            // the pad arrives 60 times a second, so match it before anything else and never log it
            if (length >= PadReceiver.PacketBytes && buffer[0] == 'C' && buffer[1] == 'P')
            {
               streamConfirmed = true;   // the PS3 has latched on; the watchdog can hold it to the normal timeout
               padReceiver.Handle(buffer, length, (IPEndPoint)sender);
               continue;
            }

            string text = Encoding.ASCII.GetString(buffer, 0, length);
            if (text.StartsWith("TIME"))
            {
               // clock sync: the PS3 pairs our clock with its own so it can measure per-stage latency
               byte[] reply = Encoding.ASCII.GetBytes("TIME " + StreamSender.NowUs);
               socket.SendTo(reply, sender);
            }
            else if (text.StartsWith("PLAY"))
            {
               if (!IsArmed) continue;   // stopped, or the encoder is broken: do not touch the desktop
               lock (streamLock)   // don't let a watchdog stop interleave with bringing a stream up
               {
                  ConnectedPs3 = ((IPEndPoint)sender).Address.ToString();
                  // the desktop must be at the streaming size BEFORE ffmpeg starts capturing it
                  keepDisplayAwake(true);
                  displayMode.MatchTo(Width, Height, Fps);
                  liveStreamer.Start((IPEndPoint)sender);   // repeat PLAYs are ignored inside
                  audioStreamer.Start((IPEndPoint)sender);
               }
            }
            else if (text.StartsWith("PADMODE "))
            {
               padReceiver.SetGamepadMode(text.Substring(8).StartsWith("gamepad"));
            }
            else if (text.StartsWith("KEY ") && length >= 5)
            {
               padReceiver.TypeKey((char)buffer[4]);   // the raw byte after "KEY " is the character
            }
            else if (text.StartsWith("CUSTOM "))
            {
               int slot;
               if (int.TryParse(text.Substring(7).Trim(), out slot)) CustomCommands.Run(slot);
            }
            else if (text.StartsWith("STOP"))
            {
               StopStreaming("the PS3 asked us to stop");
            }
            else
            {
               Log("unknown packet from " + sender + ": " + text);
            }
         }
      }

      // everything that a stream turns on gets turned off here, whoever asked - a STOP from the PS3, or
      // the PS3 vanishing. leaving any of it on would keep the desktop at 720p and the pad half-held.
      private static void StopStreaming(string why)
      {
         lock (streamLock)
         {
            bool wasStreaming = liveStreamer != null && liveStreamer.IsStreaming;
            if (liveStreamer != null) liveStreamer.Stop();
            if (audioStreamer != null) audioStreamer.Stop();
            ConnectedPs3 = null;
            streamConfirmed = false;
            padReceiver.Release();
            displayMode.Restore();
            keepDisplayAwake(false);   // idle again: let the screen sleep
            if (wasStreaming) Log("stream stopped: " + why + ". waiting for the PS3 again.");
         }
      }

      // the PS3 sends its pad 60x a second for as long as it is streaming, so silence means it is gone
      // (app closed, console off, WiFi dropped). without this the server would stream to nobody forever,
      // and never give the desktop its resolution back.
      private static void RunClientWatchdog()
      {
         while (true)
         {
            Thread.Sleep(WatchdogTickMs);
            if (liveStreamer == null || !liveStreamer.IsStreaming)
            {
               // the pump can stop on its own (every encoder failed to start, or ffmpeg died) with nothing
               // to put the desktop back. if it left the resolution switched, restore it here.
               if (displayMode.IsChanged) StopStreaming("the encoder stopped on its own");
               continue;
            }
            int timeout = streamConfirmed ? ClientTimeoutMs : StreamStartupGraceMs;
            if (SinceLastClientPacket.ElapsedMilliseconds < timeout) continue;
            StopStreaming("nothing from the PS3 for " + timeout + "ms");
         }
      }

      internal static void Log(string message)
      {
         CellStreamServer.Log.Write(message);
      }
   }
}
