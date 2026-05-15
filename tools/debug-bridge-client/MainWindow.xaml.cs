using System;
using System.IO;
using System.Windows;
using System.Windows.Media;
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
            ps3.Connected    += (s, e) => SetConnectionStatus(true);
            ps3.Disconnected += (s, e) => SetConnectionStatus(false);

            httpBridge = new HttpBridge(ps3, AppendLog);

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

        private void OnRestartXmb(object sender, RoutedEventArgs e) { SendCommand("restart-xmb"); }
        private void OnRestartPs3(object sender, RoutedEventArgs e) { SendCommand("restart-ps3"); }
        private void OnShutdown(object sender, RoutedEventArgs e) { SendCommand("shutdown"); }
        private void OnScreenshot(object sender, RoutedEventArgs e) { SendCommand("screenshot"); }
        private void OnListVshPlugins(object sender, RoutedEventArgs e) { SendCommand("vsh-plugin-list"); }

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

            string cmd = "vsh-plugin-install " + name + " " + payload.Length;
            AppendLog("ui -> " + cmd + " (" + payload.Length + " bytes)");
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                string[] lines = ps3.SendCommandWithPayload(cmd, payload);
                Dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action(() =>
                {
                    foreach (string line in lines) AppendLog("ps3 -> " + line);
                }));
            });
        }

        private void OnPluginUninstall(object sender, RoutedEventArgs e)
        {
            if (!ps3.IsConnected) { AppendLog("not connected"); return; }
            string name = PromptInput("Uninstall VSH plugin", "Plugin name (without .sprx):");
            if (string.IsNullOrEmpty(name)) return;
            SendCommand("vsh-plugin-uninstall " + name);
        }

        private void OnGetFile(object sender, RoutedEventArgs e)
        {
            if (!ps3.IsConnected) { AppendLog("not connected"); return; }
            string path = PromptInput("Get file", "PS3 path (e.g. /dev_hdd0/tmp/dbg.txt):");
            if (string.IsNullOrEmpty(path)) return;
            AppendLog("ui -> get-file " + path);
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                var dl = ps3.Download("get-file \"" + path + "\"");
                Dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action(() =>
                {
                    AppendLog("ps3 -> " + dl.Status);
                    if (dl.Data != null) AppendLog("(" + dl.Data.Length + " bytes received \u2014 fetch via http bridge to save)");
                }));
            });
        }

        private void OnDeleteFile(object sender, RoutedEventArgs e)
        {
            if (!ps3.IsConnected) { AppendLog("not connected"); return; }
            string path = PromptInput("Delete file", "PS3 path:");
            if (string.IsNullOrEmpty(path)) return;
            SendCommand("delete-file \"" + path + "\"");
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

            string cmd = "save-file \"" + ps3Path + "\" " + payload.Length;
            AppendLog("ui -> " + cmd + " (" + payload.Length + " bytes)");
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                string[] lines = ps3.SendCommandWithPayload(cmd, payload);
                Dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action(() =>
                {
                    foreach (string line in lines) AppendLog("ps3 -> " + line);
                }));
            });
        }

        // minimal input prompt — avoids a separate xaml file for one textbox.
        private string PromptInput(string title, string label)
        {
            Window w = new Window
            {
                Title = title, Width = 360, Height = 130,
                WindowStartupLocation = WindowStartupLocation.CenterOwner,
                Owner = this, ResizeMode = ResizeMode.NoResize,
                Background = (Brush)FindResource("BgBase")
            };
            var sp = new System.Windows.Controls.StackPanel { Margin = new Thickness(10) };
            sp.Children.Add(new System.Windows.Controls.TextBlock {
                Text = label, Foreground = (Brush)FindResource("FgText"), Margin = new Thickness(0,0,0,6)
            });
            var tb = new System.Windows.Controls.TextBox { Margin = new Thickness(0,0,0,8) };
            sp.Children.Add(tb);
            var btns = new System.Windows.Controls.StackPanel {
                Orientation = System.Windows.Controls.Orientation.Horizontal,
                HorizontalAlignment = HorizontalAlignment.Right
            };
            string result = null;
            var ok = new System.Windows.Controls.Button { Content = "OK", Width = 70, Margin = new Thickness(0,0,6,0), IsDefault = true };
            var cancel = new System.Windows.Controls.Button { Content = "Cancel", Width = 70, IsCancel = true };
            ok.Click += (s, e) => { result = tb.Text.Trim(); w.DialogResult = true; };
            btns.Children.Add(ok); btns.Children.Add(cancel);
            sp.Children.Add(btns);
            w.Content = sp;
            tb.Focus();
            return w.ShowDialog() == true ? result : null;
        }

        private void SendCommand(string cmd)
        {
            if (!ps3.IsConnected)
            {
                AppendLog("not connected");
                return;
            }
            AppendLog("ui -> " + cmd);
            System.Threading.ThreadPool.QueueUserWorkItem(delegate
            {
                string[] lines = ps3.SendCommand(cmd);
                Dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action(() =>
                {
                    foreach (string line in lines) AppendLog("ps3 -> " + line);
                }));
            });
        }

        public void AppendLog(string message)
        {
            if (!Dispatcher.CheckAccess())
            {
                Dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action(() => AppendLog(message)));
                return;
            }
            logBox.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + message + "\n");
            logBox.ScrollToEnd();
        }
    }
}
