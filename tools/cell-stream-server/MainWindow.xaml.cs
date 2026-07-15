using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;
using Forms = System.Windows.Forms;
using Drawing = System.Drawing;   // WPF and WinForms both have a Brush, a Color and an Icon

namespace CellStreamServer
{
   // a window onto the server, and an icon in the notification area. minimising or closing the window
   // only hides it - the server keeps running, which is the point. Exit, from the tray, is the only way
   // to stop it. no installer: unzip the folder, double click the exe; to update, replace it and restart.
   public partial class MainWindow : Window
   {
      private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
      private const int RefreshTickMs = 500;
      private const int BalloonMs = 4000;

      private readonly Forms.NotifyIcon trayIcon = new Forms.NotifyIcon();
      private readonly DispatcherTimer refreshTimer = new DispatcherTimer();
      private bool wasConnected;
      private bool wasArmed = true;
      private bool exiting;
      private string shownLog = "";

      public MainWindow()
      {
         InitializeComponent();
         SettingsText.Text = Server.SettingsSummary;
         BuildTray();

         EncoderChoice.ItemsSource = Server.AvailableEncoders;
         EncoderChoice.SelectedItem = Server.ChosenEncoder;

         StartWithWindows.IsChecked = IsStartWithWindows();
         StartWithWindows.Checked += (sender, e) => SetStartWithWindows(true);
         StartWithWindows.Unchecked += (sender, e) => SetStartWithWindows(false);

         StateChanged += (sender, e) => { if (WindowState == WindowState.Minimized) HideToTray(); };

         refreshTimer.Interval = TimeSpan.FromMilliseconds(RefreshTickMs);
         refreshTimer.Tick += (sender, e) => Refresh();
         refreshTimer.Start();
         Refresh();
      }

      private void BuildTray()
      {
         var menu = new Forms.ContextMenuStrip();
         menu.Items.Add("Show", null, (sender, e) => ShowFromTray());
         menu.Items.Add("Open log", null, (sender, e) => OpenLog());
         menu.Items.Add(new Forms.ToolStripSeparator());
         menu.Items.Add("Exit", null, (sender, e) => Exit());

         trayIcon.ContextMenuStrip = menu;
         trayIcon.DoubleClick += (sender, e) => ShowFromTray();
         trayIcon.Visible = true;
         SetTrayIcon(false);
      }

      private void OnStartStopClicked(object sender, RoutedEventArgs e)
      {
         if (Server.IsArmed) Server.Disarm("stopped by you");
         else Server.Arm();
         Refresh();
      }

      private void OnEncoderChosen(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
      {
         Server.ChosenEncoder = EncoderChoice.SelectedItem as VideoEncoder;
      }

      // grey until a PS3 turns up, green while it is streaming - with a popup either way, so you know it
      // happened without having to watch the window
      private void Refresh()
      {
         bool connected = Server.IsPs3Connected;
         string who = Server.ConnectedPs3;
         string status = !Server.IsArmed ? "Stopped" : connected ? "PS3 connected: " + who : "Waiting for a PS3 ...";

         StatusText.Text = status;
         StatusDot.Fill = (Brush)FindResource(connected ? "BrLive" : "BrIdle");
         StartStopButton.Content = Server.IsArmed ? "Stop" : "Start";
         SettingsText.Text = Server.IsArmed || Server.TripReason == null ? Server.SettingsSummary : Server.TripReason;
         trayIcon.Text = "Cell Stream - " + status;   // Windows caps the tooltip at 63 characters

         if (connected != wasConnected)
         {
            wasConnected = connected;
            SetTrayIcon(connected);
            ShowBalloon(connected ? "PS3 connected" : "PS3 disconnected",
                        connected ? who + " is streaming." : "Waiting for it to come back.");
         }

         // the fuse can trip while the window is hidden, so say so where it will be seen
         if (Server.IsArmed != wasArmed)
         {
            wasArmed = Server.IsArmed;
            if (!wasArmed && Server.TripReason != null) ShowBalloon("Streaming stopped", Server.TripReason);
         }

         if (!IsVisible) return;
         string recent = Log.GetRecent();
         if (recent == shownLog) return;
         shownLog = recent;
         LogBox.Text = recent;
         LogBox.ScrollToEnd();
      }

      // the tray icon is drawn rather than shipped: a grey dot, or a green one while a PS3 is streaming
      private void SetTrayIcon(bool connected)
      {
         Drawing.Icon previous = trayIcon.Icon;
         using (var bitmap = new Drawing.Bitmap(16, 16))
         using (Drawing.Graphics graphics = Drawing.Graphics.FromImage(bitmap))
         using (var brush = new Drawing.SolidBrush(connected ? Drawing.Color.FromArgb(0x3D, 0xD5, 0x6D)
                                                            : Drawing.Color.FromArgb(0x8A, 0x8A, 0x8A)))
         {
            graphics.SmoothingMode = Drawing.Drawing2D.SmoothingMode.AntiAlias;
            graphics.FillEllipse(brush, 2, 2, 12, 12);
            trayIcon.Icon = Drawing.Icon.FromHandle(bitmap.GetHicon());
         }
         if (previous != null) DestroyIcon(previous.Handle);
      }

      private void ShowBalloon(string title, string body)
      {
         trayIcon.BalloonTipTitle = title;
         trayIcon.BalloonTipText = body;
         trayIcon.BalloonTipIcon = Forms.ToolTipIcon.Info;
         trayIcon.ShowBalloonTip(BalloonMs);
      }

      public void HideToTray()
      {
         Hide();
         ShowInTaskbar = false;
      }

      private void ShowFromTray()
      {
         Show();
         ShowInTaskbar = true;
         WindowState = WindowState.Normal;
         Activate();
      }

      // the X button hides us; only Exit really exits
      protected override void OnClosing(CancelEventArgs e)
      {
         if (!exiting)
         {
            e.Cancel = true;
            HideToTray();
            return;
         }
         refreshTimer.Stop();
         trayIcon.Visible = false;
         trayIcon.Dispose();
         base.OnClosing(e);
      }

      private void Exit()
      {
         exiting = true;
         Close();
         Application.Current.Shutdown();   // App.OnExit puts the desktop resolution back
      }

      private static void OpenLog()
      {
         try { Process.Start(new ProcessStartInfo(Log.Path) { UseShellExecute = true }); }
         catch (Exception exception) { Log.Write("could not open the log: " + exception.Message); }
      }

      private static string ExePath { get { return Assembly.GetExecutingAssembly().Location; } }
      private static string RunValueName { get { return Path.GetFileNameWithoutExtension(ExePath); } }

      private static bool IsStartWithWindows()
      {
         try
         {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RunKey))
               return key != null && key.GetValue(RunValueName) != null;
         }
         catch { return false; }
      }

      private static void SetStartWithWindows(bool wanted)
      {
         try
         {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RunKey, true))
            {
               if (key == null) return;
               if (wanted) key.SetValue(RunValueName, "\"" + ExePath + "\" " + App.StartMinimizedSwitch);
               else key.DeleteValue(RunValueName, false);
            }
            Log.Write(wanted ? "will start with Windows" : "will no longer start with Windows");
         }
         catch (Exception exception)
         {
            Log.Write("could not change the Windows start-up setting: " + exception.Message);
         }
      }

      [System.Runtime.InteropServices.DllImport("user32.dll")]
      private static extern bool DestroyIcon(IntPtr handle);
   }
}
