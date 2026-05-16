using System;
using System.IO;
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

        public MainWindow()
        {
            InitializeComponent();

            ps3 = new Ps3Connection();
            ps3.Connected    += (s, e) => OnConnected();
            ps3.Disconnected += (s, e) => SetConnectionStatus(false);

            httpBridge = new HttpBridge(ps3, AppendLog);
            // mirror http-driven captures to the canvas so any /capture call
            // (curl, browser, scripts) shows up live in the screen tab.
            httpBridge.CaptureReceived += (x, y, w, h, argb) =>
                Dispatcher.BeginInvoke(DispatcherPriority.Normal,
                    new Action(() => DrawCapture(x, y, w, h, argb)));

            Loaded += OnLoaded;
            Closed += OnClosed;
        }

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            AppendLog("http bridge on http://localhost:8786/");
            httpBridge.Start();
            AppendLog("connecting to " + ps3.Host + "...");
            ps3.StartAutoConnect();
        }

        private void OnClosed(object sender, EventArgs e)
        {
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
                AppendLog(connected ? "connected" : "disconnected");
            }));
        }

        private void OnRestartXmb(object sender, RoutedEventArgs e) { RunCommand("restart-xmb"); }
        private void OnRestartPs3(object sender, RoutedEventArgs e) { RunCommand("restart-ps3"); }
        private void OnShutdown(object sender, RoutedEventArgs e) { RunCommand("shutdown"); }

        // capture pipeline test harness. wired to Commands -> Screenshot.
        // routed through the http bridge (just like an external caller)
        // so there is exactly one capture path: /capture -> CaptureReceived
        // event -> DrawCapture. samples a 320x180 patch at screen centre;
        // plugin clips to display bounds so this is safe at 720p too.
        private void OnScreenshot(object sender, RoutedEventArgs e)
        {
            if (!ps3.IsConnected) { AppendLog("not connected"); return; }
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                try
                {
                    using (var wc = new System.Net.WebClient())
                        wc.DownloadData("http://localhost:" + HttpBridge.Port + "/capture?x=800&y=450&w=320&h=180");
                }
                catch (Exception ex) { AppendLog("screenshot failed: " + ex.Message); }
            });
        }

        // vram byte order is A,R,G,B per pixel (big-endian ARGB word). wpf's
        // Bgra32 expects B,G,R,A, so swap. forced opaque alpha because vram
        // alpha is unreliable. partial captures get a thin lime border so
        // they're visible against the dark canvas; full-screen captures get
        // none (they cover everything). thickness is in canvas units, so the
        // Viewbox scales it with the rest of the tile.
        private void DrawCapture(int x, int y, int w, int h, byte[] argb)
        {
            byte[] bgra = new byte[argb.Length];
            for (int i = 0; i < argb.Length; i += 4)
            {
                bgra[i + 0] = argb[i + 3];
                bgra[i + 1] = argb[i + 2];
                bgra[i + 2] = argb[i + 1];
                bgra[i + 3] = 0xFF;
            }
            var bmp = BitmapSource.Create(w, h, 96, 96, PixelFormats.Bgra32, null, bgra, w * 4);
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

        private void OnListVshPlugins(object sender, RoutedEventArgs e) { RunCommand("vsh-plugin-list"); }

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

        private void OnPluginUninstall(object sender, RoutedEventArgs e)
        {
            if (!ps3.IsConnected) { AppendLog("not connected"); return; }
            string name = PromptInput("Uninstall VSH plugin", "Plugin name (without .sprx):");
            if (string.IsNullOrEmpty(name)) return;
            RunCommand("vsh-plugin-uninstall " + name);
        }

        private void OnGetFile(object sender, RoutedEventArgs e)
        {
            if (!ps3.IsConnected) { AppendLog("not connected"); return; }
            string path = PromptInput("Get file", "PS3 path (e.g. /dev_hdd0/tmp/dbg.txt):");
            if (string.IsNullOrEmpty(path)) return;
            RunCommand("get-file \"" + path + "\"");
        }

        private void OnDeleteFile(object sender, RoutedEventArgs e)
        {
            if (!ps3.IsConnected) { AppendLog("not connected"); return; }
            string path = PromptInput("Delete file", "PS3 path:");
            if (string.IsNullOrEmpty(path)) return;
            RunCommand("delete-file \"" + path + "\"");
        }

        private void OnSaveFile(object sender, RoutedEventArgs e)
        {
            if (!ps3.IsConnected) { AppendLog("not connected"); return; }
            OpenFileDialog dlg = new OpenFileDialog();
            dlg.Title = "Select file to upload";
            if (dlg.ShowDialog(this) != true) return;
            string localPath = dlg.FileName;
            string ps3Path = PromptInput("Save file",
                "PS3 destination path (e.g. /dev_hdd0/tmp/" + Path.GetFileName(localPath) + "):");
            if (string.IsNullOrEmpty(ps3Path)) return;
            byte[] payload;
            try { payload = File.ReadAllBytes(localPath); }
            catch (Exception ex) { AppendLog("read failed: " + ex.Message); return; }
            RunCommand("save-file \"" + ps3Path + "\" " + payload.Length, payload);
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
        // (e.g. vsh-plugin-list) get one log line per record, with the
        // continuation lines indented under the "ps3 ->" prefix so they
        // line up visually.
        private void RunCommand(string cmd, byte[] upload = null)
        {
            if (!ps3.IsConnected) { AppendLog("not connected"); return; }
            AppendLog("ui -> " + cmd + (upload != null ? " (" + upload.Length + " bytes)" : ""));
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                string reply = SendText(cmd, upload);
                Dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action(() =>
                {
                    string[] lines = reply.Split('\n');
                    AppendLog("ps3 -> " + lines[0]);
                    for (int i = 1; i < lines.Length; i++)
                        if (lines[i].Length > 0) AppendLog("       " + lines[i]);
                }));
            });
        }

        // text-mode helper: format the reply as "OK [text]" / "ERR [text]"
        // for display in the log box. binary commands (capture, get-file)
        // get the Ps3Reply payload directly via ps3.SendCommand.
        private string SendText(string cmd, byte[] upload = null)
        {
            Ps3Reply r = ps3.SendCommand(cmd, upload);
            string prefix = r.Ok ? "OK" : "ERR";
            if (r.Payload.Length == 0) return prefix;
            return prefix + " " + r.AsText().TrimEnd('\n');
        }

        private void AppendLog(string msg)
        {
            if (!Dispatcher.CheckAccess())
            {
                Dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action<string>(AppendLog), msg);
                return;
            }
            logBox.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + msg + Environment.NewLine);
            logBox.ScrollToEnd();
        }
    }
}
