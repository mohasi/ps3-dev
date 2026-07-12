using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;

namespace DebugBridgeClient
{
   public partial class MainWindow : Window
   {
      private Ps3Connection ps3;
      private HttpBridge httpBridge;

      // batched log pipeline: producers (any thread) enqueue lines, a
      // single dispatcher timer drains the queue per tick.
      private readonly Queue<string> logsQueue = new Queue<string>();
      private readonly object        queueLock = new object();

      // full logs history (ui thread only) so the filter box can re-show
      // lines that were hidden while a filter was active. capped at the
      // same char budget as the visible box.
      private readonly List<string> logsHistory      = new List<string>();
      private int                   logsHistoryChars = 0;
      private DispatcherTimer logFlushTimer;
      private const int LogMaxChars  = 512 * 1024;
      private const int LogTrimChunk =  64 * 1024;

      public MainWindow()
      {
         InitializeComponent();

         ps3 = new Ps3Connection();
         ps3.Connected    += (s, e) => OnConnected();
         ps3.Disconnected += (s, e) => SetConnectionStatus(false);
         ps3.LogReceived  += OnPs3Log;

         httpBridge = new HttpBridge(ps3, AppendLog);
         // mirror http-driven captures to the canvas so any /capture call
         // (curl, browser, scripts) shows up live in the screen tab.
         httpBridge.CaptureReceived += (x, y, w, h, argb) =>
            Dispatcher.BeginInvoke(DispatcherPriority.Normal,
                new Action(() => DrawCapture(x, y, w, h, argb)));

         logFlushTimer = new DispatcherTimer(DispatcherPriority.Background) {
            Interval = TimeSpan.FromMilliseconds(33)
         };
         logFlushTimer.Tick += (s, e) => FlushLogs();
         logFlushTimer.Start();

         Loaded += OnLoaded;
         Closed += OnClosed;
      }

      private void OnLoaded(object sender, RoutedEventArgs e)
      {
         AppendLog("http bridge on http://localhost:8786/");
         httpBridge.Start();
         modulesView.Attach(ps3);
         traceView.Attach(ps3);
         AppendLog("connecting to " + ps3.Hosts + "...");
         ps3.StartAutoConnect();
      }

      private void OnClosed(object sender, EventArgs e)
      {
         logFlushTimer.Stop();
         httpBridge.Stop();
         ps3.Disconnect();
      }

      private void OnConnected()
      {
         SetConnectionStatus(true);
         // size the canvas to the ps3's actual framebuffer so capture
         // coordinates are screen-relative regardless of 720p/1080p output.
         System.Threading.ThreadPool.QueueUserWorkItem(delegate
         {
             string reply = SendText("display-info");
             int w, h;
             if (TryParseDisplayInfo(reply, out w, out h))
             {
                 Dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action(() =>
                 {
                     screenCanvas.Width  = w;
                     screenCanvas.Height = h;
                     AppendLog("display: " + w + "x" + h);
                 }));
             }
         });
      }

      // "OK <w> <h> <pitch> <depth>" -> w, h.
      private static bool TryParseDisplayInfo(string reply, out int w, out int h)
      {
         w = 0; h = 0;
         if (reply == null) return false;
         string status = reply.Split('\n')[0];
         if (!status.StartsWith("OK ")) return false;
         string[] parts = status.Substring(3).Split(' ');
         return parts.Length >= 2 && int.TryParse(parts[0], out w) && int.TryParse(parts[1], out h);
      }

      private void SetConnectionStatus(bool connected)
      {
         Dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action(() =>
         {
             statusDot.Fill  = new SolidColorBrush(connected
                 ? Color.FromRgb(0x22, 0xbb, 0x22)
                 : Color.FromRgb(0xdd, 0x22, 0x22));
             statusText.Text = connected ? "Connected" : "Disconnected";
             commandsMenu.IsEnabled = connected;   // every Commands entry needs the ps3
             AppendLog(connected ? ("connected to " + ps3.Host) : "disconnected");
         }));
      }

      private void OnRestartXmb(object sender, RoutedEventArgs e) { RunCommand("restart-xmb"); }
      private void OnRestartPs3(object sender, RoutedEventArgs e) { RunCommand("restart-ps3"); }
      private void OnShutdown(object sender, RoutedEventArgs e) { RunCommand("shutdown"); }

      // wired to Commands -> Screenshot. captures the whole screen (display-info
      // gives the current 720p/1080p size), draws it on the canvas, then prompts
      // to save it as a PNG. drawing happens before the dialog so Cancel still
      // leaves the shot on the canvas.
      private void OnScreenshot(object sender, RoutedEventArgs e)
      {
         if (!ps3.IsConnected) { AppendLog("not connected"); return; }
         System.Threading.ThreadPool.QueueUserWorkItem(delegate
         {
             int w, h;
             if (!TryParseDisplayInfo(SendText("display-info"), out w, out h))
             {
                 AppendLog("screenshot failed: no display info");
                 return;
             }
             Ps3Reply r = ps3.SendCommand("capture 0 0 " + w + " " + h);
             if (!r.Ok || r.Payload.Length != w * h * 4)
             {
                 AppendLog("screenshot failed: " + (r.Ok ? "short capture" : r.AsText().TrimEnd('\n')));
                 return;
             }
             byte[] argb = r.Payload;
             Dispatcher.BeginInvoke(new Action(delegate
             {
                 DrawCapture(0, 0, w, h, argb);
                 SaveScreenshot(w, h, argb);
             }));
         });
      }

      // prompt for a PNG destination, default name a timestamp. Cancel = discard
      // (the shot is already on the canvas). encodes the shared BGRA bitmap.
      private void SaveScreenshot(int w, int h, byte[] argb)
      {
         SaveFileDialog dlg = new SaveFileDialog();
         dlg.Title    = "Save screenshot";
         dlg.FileName = "screenshot-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".png";
         dlg.Filter   = "PNG image (*.png)|*.png|All files (*.*)|*.*";
         if (dlg.ShowDialog(this) != true) return;
         try
         {
             var encoder = new PngBitmapEncoder();
             encoder.Frames.Add(BitmapFrame.Create(CaptureToBitmap(w, h, argb)));
             using (var file = File.Create(dlg.FileName)) encoder.Save(file);
             AppendLog("saved screenshot to " + dlg.FileName);
         }
         catch (Exception ex) { AppendLog("save failed: " + ex.Message); }
      }

      // vram byte order is A,R,G,B per pixel (big-endian ARGB word). wpf's
      // Bgra32 expects B,G,R,A, so swap. forced opaque alpha because vram
      // alpha is unreliable.
      private static BitmapSource CaptureToBitmap(int w, int h, byte[] argb)
      {
         byte[] bgra = new byte[argb.Length];
         for (int i = 0; i < argb.Length; i += 4)
         {
            bgra[i + 0] = argb[i + 3];
            bgra[i + 1] = argb[i + 2];
            bgra[i + 2] = argb[i + 1];
            bgra[i + 3] = 0xFF;
         }
         return BitmapSource.Create(w, h, 96, 96, PixelFormats.Bgra32, null, bgra, w * 4);
      }

      // partial captures get a thin lime border so they're visible against the
      // dark canvas; full-screen captures get none (they cover everything).
      // thickness is in canvas units, so the Viewbox scales it with the tile.
      private void DrawCapture(int x, int y, int w, int h, byte[] argb)
      {
         var bmp = CaptureToBitmap(w, h, argb);
         var img = new Image { Source = bmp, Width = w, Height = h, Stretch = Stretch.Fill };
         UIElement tile = img;
         bool fullScreen = x == 0 && y == 0 && w >= screenCanvas.Width && h >= screenCanvas.Height;
         if (!fullScreen)
         {
            tile = new Border
            {
               Child = img,
               BorderBrush = Brushes.Lime,
               BorderThickness = new Thickness(2),
               Width = w, Height = h
            };
         }
         Canvas.SetLeft(tile, x);
         Canvas.SetTop(tile, y);
         screenCanvas.Children.Add(tile);
      }

      // walk a ps3 subtree and have the bridge write a sha1'd snapshot to
      // /dev_hdd0/tmp/stat-tree.txt. used for before/after install diffs to
      // find what xmb registration touches beyond the pkg extraction itself.
      // fire-and-forget: the bridge logs progress, and the operator pulls the
      // file via Files -> Pull File... once the OK comes back.
      private void OnStatTree(object sender, RoutedEventArgs e)
      {
         if (!ps3.IsConnected) { AppendLog("not connected"); return; }
         string root = PromptInput("Stat tree", "Root path:");
         if (string.IsNullOrEmpty(root)) return;
         RunCommand("stat-tree \"" + root + "\"", null, StatTreeTimeoutMs);
      }

      // stat-tree hashes every file under <root>; on /dev_hdd0 that walks
      // 10k+ entries and currently runs ~40 s, well under this ceiling.
      // shared with HttpBridge so /stat-tree and the UI agree.
      public const int StatTreeTimeoutMs = 5 * 60 * 1000;

      private void OnPluginInstall(object sender, RoutedEventArgs e)
      {
         if (!ps3.IsConnected) { AppendLog("not connected"); return; }
         OpenFileDialog dlg = new OpenFileDialog();
         dlg.Filter = "PS3 PRX (*.sprx)|*.sprx|All files (*.*)|*.*";
         dlg.Title = "Install VSH plugin";
         if (dlg.ShowDialog(this) != true) return;
         string path = dlg.FileName;
         string name = Path.GetFileNameWithoutExtension(path);
         byte[] payload;
         try { payload = File.ReadAllBytes(path); }
         catch (Exception ex) { AppendLog("read failed: " + ex.Message); return; }
         RunCommand("vsh-plugin-install " + name + " " + payload.Length, payload);
      }

      // ship the pkg bytes to the bridge; it stages to /dev_hdd0/packages/<name>.pkg
      // (the .pkg suffix is appended by buildStagePath on the ps3 side) and then
      // extracts to /dev_hdd0/game/<TITLE_ID>/. clean=1 mirrors xmb reinstall.
      private void OnPackageInstall(object sender, RoutedEventArgs e)
      {
         if (!ps3.IsConnected) { AppendLog("not connected"); return; }
         OpenFileDialog dlg = new OpenFileDialog();
         dlg.Filter = "PS3 package (*.pkg)|*.pkg";
         dlg.Title = "Install package";
         if (dlg.ShowDialog(this) != true) return;
         string path = dlg.FileName;
         string name = Path.GetFileNameWithoutExtension(path);
         byte[] payload;
         try { payload = File.ReadAllBytes(path); }
         catch (Exception ex) { AppendLog("read failed: " + ex.Message); return; }
         RunCommand("pkg-install " + name + " 1 " + payload.Length, payload);
      }

      private void OnPluginUninstall(object sender, RoutedEventArgs e)
      {
         if (!ps3.IsConnected) { AppendLog("not connected"); return; }
         string name = PromptInput("Uninstall VSH plugin", "Plugin name (without .sprx):");
         if (string.IsNullOrEmpty(name)) return;
         RunCommand("vsh-plugin-uninstall " + name);
      }

      // single handler for both the "Custom..." item and the preset
      // submenu entries: presets carry the PS3 path in MenuItem.Tag,
      // Custom... has no Tag and falls through to a prompt.
      private void OnPullFile(object sender, RoutedEventArgs e)
      {
         if (!ps3.IsConnected) { AppendLog("not connected"); return; }
         MenuItem mi = sender as MenuItem;
         string path = mi != null ? mi.Tag as string : null;
         if (string.IsNullOrEmpty(path))
            path = PromptInput("Pull file", "PS3 path (e.g. /dev_hdd0/tmp/dbg.txt):");
         if (string.IsNullOrEmpty(path)) return;
         FetchFileToDisk(path);
      }

      // run pull-file on a worker thread, then prompt for a local save
      // destination on the UI thread. payload is written raw - no
      // text decoding - so binary files (stat-tree.txt, trace bins)
      // round-trip cleanly. cancel = silent discard.
      private void FetchFileToDisk(string ps3Path)
      {
         AppendLog("ui -> pull-file \"" + ps3Path + "\"");
         System.Threading.ThreadPool.QueueUserWorkItem(delegate
         {
             Ps3Reply r = ps3.SendCommand("pull-file \"" + ps3Path + "\"");
             if (!r.Ok)
             {
                 AppendLog("ps3 -> ERR " + r.AsText().TrimEnd('\n'));
                 return;
             }
             byte[] payload = r.Payload;
             Dispatcher.BeginInvoke(new Action(delegate
             {
                 SaveFileDialog dlg = new SaveFileDialog();
                 dlg.Title    = "Save " + ps3Path;
                 dlg.FileName = System.IO.Path.GetFileName(ps3Path);
                 dlg.Filter   = "All files (*.*)|*.*";
                 if (dlg.ShowDialog(this) != true) return;
                 try
                 {
                     File.WriteAllBytes(dlg.FileName, payload);
                     AppendLog("ps3 -> OK saved " + payload.Length + " bytes to " + dlg.FileName);
                 }
                 catch (Exception ex)
                 {
                     AppendLog("save failed: " + ex.Message);
                 }
             }));
         });
      }

      private void OnDeleteFile(object sender, RoutedEventArgs e)
      {
         if (!ps3.IsConnected) { AppendLog("not connected"); return; }
         MenuItem mi = sender as MenuItem;
         string path = mi != null ? mi.Tag as string : null;
         if (string.IsNullOrEmpty(path))
            path = PromptInput("Delete file", "PS3 path:");
         if (string.IsNullOrEmpty(path)) return;
         RunCommand("delete-file \"" + path + "\"");
      }

      private void OnPushFile(object sender, RoutedEventArgs e)
      {
         if (!ps3.IsConnected) { AppendLog("not connected"); return; }
         OpenFileDialog dlg = new OpenFileDialog();
         dlg.Title = "Select file to upload";
         if (dlg.ShowDialog(this) != true) return;
         string localPath = dlg.FileName;
         string ps3Path = PromptInput("Push file",
             "PS3 destination path (e.g. /dev_hdd0/tmp/" + Path.GetFileName(localPath) + "):");
         if (string.IsNullOrEmpty(ps3Path)) return;
         byte[] payload;
         try { payload = File.ReadAllBytes(localPath); }
         catch (Exception ex) { AppendLog("read failed: " + ex.Message); return; }
         RunCommand("push-file \"" + ps3Path + "\" " + payload.Length, payload);
      }

      // wipe the logs view and its kept history so the next session starts
      // from a clean slate — useful when reproducing a specific sequence.
      private void OnClearLogs(object sender, RoutedEventArgs e)
      {
         logsBox.Clear();
         logsHistory.Clear();
         logsHistoryChars = 0;
      }

      // minimal input prompt - avoids a separate xaml file for one textbox.
      private string PromptInput(string title, string label)
      {
         Window w = new Window
         {
            Title = title, Width = 360, Height = 130,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Owner = this, ResizeMode = ResizeMode.NoResize,
            Background = (Brush)FindResource("BgBase")
         };
         var sp = new StackPanel { Margin = new Thickness(10) };
         sp.Children.Add(new TextBlock {
             Text = label, Foreground = (Brush)FindResource("FgText"), Margin = new Thickness(0,0,0,6)
         });
         var tb = new TextBox { Margin = new Thickness(0,0,0,8) };
         sp.Children.Add(tb);
         var btns = new StackPanel {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right
         };
         string result = null;
         var ok = new Button { Content = "OK", Width = 70, Margin = new Thickness(0,0,6,0), IsDefault = true };
         var cancel = new Button { Content = "Cancel", Width = 70, IsCancel = true };
         ok.Click += (s, e) => { result = tb.Text.Trim(); w.DialogResult = true; };
         btns.Children.Add(ok); btns.Children.Add(cancel);
         sp.Children.Add(btns);
         w.Content = sp;
         tb.Focus();
         return w.ShowDialog() == true ? result : null;
      }

      // run a text-mode command and log the reply. multi-line payloads
      // (e.g. module-list, module-inspect) get one log line per record,
      // continuation lines indented under "ps3 ->" so they line up.
      // AppendLog is thread-safe (queue + lock, drained by the dispatcher
      // timer), so we log straight from the worker — no extra UI marshal.
      private void RunCommand(string cmd, byte[] upload = null, int timeoutMs = 10000)
      {
         if (!ps3.IsConnected) { AppendLog("not connected"); return; }
         AppendLog("ui -> " + cmd + (upload != null ? " (" + upload.Length + " bytes)" : ""));
         System.Threading.ThreadPool.QueueUserWorkItem(delegate
         {
             string reply = SendText(cmd, upload, timeoutMs);
             string[] lines = reply.Split('\n');
             AppendLog("ps3 -> " + lines[0]);
             for (int i = 1; i < lines.Length; i++)
                 if (lines[i].Length > 0) AppendLog("       " + lines[i]);
         });
      }

      // text-mode helper: format the reply as "OK [text]" / "ERR [text]"
      // for display in the log box. binary commands (capture, pull-file)
      // get the Ps3Reply payload directly via ps3.SendCommand.
      private string SendText(string cmd, byte[] upload = null, int timeoutMs = 10000)
      {
         Ps3Reply r = ps3.SendCommand(cmd, upload, timeoutMs);
         string prefix = r.Ok ? "OK" : "ERR";
         if (r.Payload.Length == 0) return prefix;
         return prefix + " " + r.AsText().TrimEnd('\n');
      }

      // host-side chatter (command/response, connect, disconnect) goes into
      // the same logs box as ps3 lines, marked "---" so it stands apart.
      // no host timestamp — ps3 lines carry their own dbg.h stamp.
      private void AppendLog(string msg)
      {
         lock (queueLock) logsQueue.Enqueue("--- " + msg);
      }

      // every line that came in over the dbg.h LOG pipeline (from any
      // ps3-side plugin including sdb itself).
      private void OnPs3Log(string line)
      {
         lock (queueLock) logsQueue.Enqueue(line.TrimEnd('\r', '\n'));
      }

      // drain the queue into one AppendText + ScrollToEnd per tick — one
      // dispatcher post per line freezes WPF; one per tick handles
      // thousands/sec.
      private void FlushLogs()
      {
         string[] lines;
         lock (queueLock) lines = DrainQueue(logsQueue);
         AppendLogsBatch(lines);
      }

      // remember every line in the history (for re-filtering), but only
      // append lines matching the current filter to the box.
      private void AppendLogsBatch(string[] lines)
      {
         if (lines == null || lines.Length == 0) return;

         // grow the history, then chop whole lines off the front once we
         // cross the same char budget the visible box uses
         foreach (string line in lines) { logsHistory.Add(line); logsHistoryChars += line.Length + 1; }
         int drop = 0;
         while (logsHistoryChars > LogMaxChars) { logsHistoryChars -= logsHistory[drop].Length + 1; drop++; }
         if (drop > 0) logsHistory.RemoveRange(0, drop);

         // append matching lines in one AppendText, then cap the box at
         // LogMaxChars by chopping LogTrimChunk off the front — keeps
         // TextBox layout cheap under sustained traffic
         string[] visible = Array.FindAll(lines, MatchesLogsFilter);
         if (visible.Length == 0) return;
         StringBuilder sb = new StringBuilder(visible.Length * 64);
         foreach (string line in visible) sb.Append(line).Append('\n');
         logsBox.AppendText(sb.ToString());
         if (logsBox.Text.Length > LogMaxChars) {
            int cut = logsBox.Text.Length - (LogMaxChars - LogTrimChunk);
            logsBox.Text = logsBox.Text.Substring(cut);
            logsBox.CaretIndex = logsBox.Text.Length;
         }
         logsBox.ScrollToEnd();
      }

      private bool MatchesLogsFilter(string line)
      {
         string filter = logsFilterBox.Text;
         return filter.Length == 0 || line.IndexOf(filter, StringComparison.OrdinalIgnoreCase) >= 0;
      }

      // re-show the whole history through the new filter
      private void OnLogsFilterChanged(object sender, TextChangedEventArgs e)
      {
         logsFilterPlaceholder.Visibility = logsFilterBox.Text.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
         StringBuilder sb = new StringBuilder();
         foreach (string line in logsHistory)
            if (MatchesLogsFilter(line)) sb.Append(line).Append('\n');
         logsBox.Text = sb.ToString();
         logsBox.ScrollToEnd();
      }

      private void OnCopyLogs(object sender, RoutedEventArgs e)
      {
         try { Clipboard.SetText(logsBox.Text); } catch (Exception) { }  // clipboard can be busy; ignore
      }

      private static string[] DrainQueue(Queue<string> q)
      {
         if (q.Count == 0) return null;
         string[] a = q.ToArray();
         q.Clear();
         return a;
      }
   }
}
