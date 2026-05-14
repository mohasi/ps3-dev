using System;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;

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
                string reply = ps3.SendCommand(cmd);
                Dispatcher.BeginInvoke(DispatcherPriority.Normal, new Action(() =>
                {
                    AppendLog("ps3 -> " + (reply ?? "no response"));
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
            logList.Items.Add(DateTime.Now.ToString("HH:mm:ss") + "  " + message);
            if (logList.Items.Count > 0)
                logList.ScrollIntoView(logList.Items[logList.Items.Count - 1]);
            while (logList.Items.Count > 5000)
                logList.Items.RemoveAt(0);
        }
    }
}
