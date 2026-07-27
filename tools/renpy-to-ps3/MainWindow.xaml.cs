using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;
using System.Windows;
using System.Windows.Threading;

namespace RenpyToPs3
{
   // thin GUI over Program.Run: the form builds the same argument list the old
   // command line took, and everything the tool prints lands in the log pane.
   public partial class MainWindow : Window
   {
      private sealed class ToolTask
      {
         public string Name;
         public string Verb;
         public string InputLabel;
         public string InputHint;         // placeholder shown inside the empty input box
         public string InputFileFilter;   // null = the input Browse button picks a folder instead
         public bool InputAllowsFolder;
         public bool HasOutput;
         public string OutputHint;
         public string OutputFileFilter;  // null with HasOutput set = output is a folder
         public bool OutputRequired;
         public bool IsPack;
         public bool IsAst;
      }

      private static readonly ToolTask[] Tasks =
      {
         new ToolTask { Name = "Convert a game to a PS3 bundle (.rpk)", Verb = "pack", InputLabel = "Game folder",
                        InputHint = "ren'py 'game' folder", InputAllowsFolder = true, HasOutput = true, OutputRequired = true,
                        OutputHint = ".rpk file to output as", OutputFileFilter = "PS3 bundle (*.rpk)|*.rpk", IsPack = true },
         new ToolTask { Name = "Check if a game is convertible", Verb = "info", InputLabel = "Game folder",
                        InputHint = "ren'py 'game' folder", InputAllowsFolder = true },
         new ToolTask { Name = "Compile scripts to bytecode", Verb = "compile", InputLabel = "Scripts",
                        InputHint = "'game' folder or a single .rpyc file", InputAllowsFolder = true, HasOutput = true,
                        OutputHint = ".rbc file to output as (optional)", OutputFileFilter = "bytecode (*.rbc)|*.rbc" },
         new ToolTask { Name = "List an archive's contents", Verb = "list", InputLabel = "Archive",
                        InputHint = ".rpa archive file", InputFileFilter = "Ren'Py archive (*.rpa)|*.rpa" },
         new ToolTask { Name = "Extract an archive to a folder", Verb = "extract", InputLabel = "Archive",
                        InputHint = ".rpa archive file", InputFileFilter = "Ren'Py archive (*.rpa)|*.rpa",
                        HasOutput = true, OutputHint = "folder to extract into", OutputRequired = true },
         new ToolTask { Name = "Inspect a bundle (.rpk)", Verb = "rpk", InputLabel = "Bundle",
                        InputHint = ".rpk bundle file", InputFileFilter = "PS3 bundle (*.rpk)|*.rpk" },
         new ToolTask { Name = "Show a script as text", Verb = "script", InputLabel = "Script",
                        InputHint = ".rpyc script file", InputFileFilter = "Ren'Py script (*.rpyc)|*.rpyc" },
         new ToolTask { Name = "Dump a script's raw tree", Verb = "ast", InputLabel = "Script",
                        InputHint = ".rpyc script file", InputFileFilter = "Ren'Py script (*.rpyc)|*.rpyc", IsAst = true },
         new ToolTask { Name = "Dump a script's animation nodes", Verb = "atldump", InputLabel = "Script",
                        InputHint = ".rpyc script file", InputFileFilter = "Ren'Py script (*.rpyc)|*.rpyc" },
      };

      private readonly object pendingLock = new object();
      private readonly StringBuilder pendingLog = new StringBuilder();

      public MainWindow()
      {
         InitializeComponent();

         // everything Program prints via Console goes to the log pane
         var writer = new LogWriter(this);
         Console.SetOut(writer);
         Console.SetError(writer);

         // batch console output into the textbox a few times a second (pack prints thousands of lines)
         var flushTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
         flushTimer.Tick += (s, e) => FlushLog();
         flushTimer.Start();

         foreach (ToolTask task in Tasks) TaskChoice.Items.Add(task.Name);
         TaskChoice.SelectedIndex = 0;
      }

      private ToolTask CurrentTask { get { return Tasks[TaskChoice.SelectedIndex]; } }

      private void OnTaskChosen(object sender, EventArgs e)
      {
         ToolTask task = CurrentTask;
         InputLabel.Text = task.InputLabel + ":";
         InputHint.Text = task.InputHint;
         OutputRow.Visibility = task.HasOutput ? Visibility.Visible : Visibility.Collapsed;
         OutputHint.Text = task.OutputHint ?? "";
         PackOptionsRow.Visibility = task.IsPack ? Visibility.Visible : Visibility.Collapsed;
         AstLabelRow.Visibility = task.IsAst ? Visibility.Visible : Visibility.Collapsed;
         StatusText.Text = "";
         OnPathTextChanged(null, null);
      }

      // placeholders hide as soon as their box has text
      private void OnPathTextChanged(object sender, RoutedEventArgs e)
      {
         InputHint.Visibility = InputBox.Text.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
         OutputHint.Visibility = OutputBox.Text.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
      }

      // browse handlers

      private void OnBrowseInput(object sender, RoutedEventArgs e)
      {
         ToolTask task = CurrentTask;
         if (task.InputAllowsFolder)
         {
            string folder = PickFolder(InputBox.Text);
            if (folder != null) InputBox.Text = folder;
         }
         else
         {
            var dialog = new Microsoft.Win32.OpenFileDialog { Filter = task.InputFileFilter };
            if (dialog.ShowDialog() == true) InputBox.Text = dialog.FileName;
         }
      }

      private void OnBrowseOutput(object sender, RoutedEventArgs e)
      {
         ToolTask task = CurrentTask;
         if (task.OutputFileFilter != null)
         {
            var dialog = new Microsoft.Win32.SaveFileDialog { Filter = task.OutputFileFilter };
            if (dialog.ShowDialog() == true) OutputBox.Text = dialog.FileName;
         }
         else
         {
            string folder = PickFolder(OutputBox.Text);
            if (folder != null) OutputBox.Text = folder;
         }
      }

      private static string PickFolder(string startPath)
      {
         using (var dialog = new System.Windows.Forms.FolderBrowserDialog())
         {
            if (Directory.Exists(startPath)) dialog.SelectedPath = startPath;
            return dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK ? dialog.SelectedPath : null;
         }
      }

      // run

      private void OnRun(object sender, RoutedEventArgs e)
      {
         ToolTask task = CurrentTask;
         string input = InputBox.Text.Trim();
         string output = OutputBox.Text.Trim();

         // validate the form before touching the engine
         if (input.Length == 0) { StatusText.Text = "pick the " + task.InputLabel.ToLower() + " first"; return; }
         if (task.OutputRequired && output.Length == 0) { StatusText.Text = "pick the output first"; return; }

         // build the argument list exactly as the command line took it
         var args = new List<string> { task.Verb, input };
         if (task.HasOutput && output.Length > 0) args.Add(output);
         if (task.IsPack)
         {
            int maxEdge;
            if (!int.TryParse(MaxEdgeBox.Text.Trim(), out maxEdge) || maxEdge < 16) { StatusText.Text = "max image size must be a number (16 or more)"; return; }
            args.Add("--max"); args.Add(maxEdge.ToString());
            if (AsciiTextCheck.IsChecked == true) args.Add("--ascii-text");
            if (NoCacheCheck.IsChecked == true) args.Add("--no-cache");
            if (ClearCacheCheck.IsChecked == true) args.Add("--clear-cache");
         }
         if (task.IsAst)
         {
            string label = AstLabelBox.Text.Trim();
            if (label.Length > 0) args.Add(label);
         }

         Console.WriteLine("> renpy-to-ps3 " + JoinForDisplay(args));
         RunButton.IsEnabled = false;
         StatusText.Text = "running ...";

         string[] argArray = args.ToArray();
         var worker = new Thread(() =>
         {
            int exitCode = Program.Run(argArray);
            Dispatcher.BeginInvoke(new Action(() =>
            {
               RunButton.IsEnabled = true;
               StatusText.Text = exitCode == 0 ? "done" : "failed (see log)";
            }));
         }) { IsBackground = true };
         worker.Start();
      }

      private static string JoinForDisplay(List<string> args)
      {
         var line = new StringBuilder();
         foreach (string arg in args)
         {
            if (line.Length > 0) line.Append(' ');
            line.Append(arg.IndexOf(' ') >= 0 ? "\"" + arg + "\"" : arg);
         }
         return line.ToString();
      }

      // log pane

      private void OnClearLog(object sender, RoutedEventArgs e)
      {
         lock (pendingLock) pendingLog.Length = 0;
         LogView.Clear();
      }

      private void FlushLog()
      {
         string chunk = null;
         lock (pendingLock)
         {
            if (pendingLog.Length > 0) { chunk = pendingLog.ToString(); pendingLog.Length = 0; }
         }
         if (chunk == null) return;
         LogView.AppendText(chunk);
         LogView.ScrollToEnd();
      }

      private sealed class LogWriter : TextWriter
      {
         private readonly MainWindow owner;
         public LogWriter(MainWindow owner) { this.owner = owner; }
         public override Encoding Encoding { get { return Encoding.UTF8; } }
         public override void Write(char c) { lock (owner.pendingLock) owner.pendingLog.Append(c); }
         public override void Write(string s) { lock (owner.pendingLock) owner.pendingLog.Append(s); }
         public override void WriteLine(string s) { lock (owner.pendingLock) owner.pendingLog.Append(s).Append("\r\n"); }
      }
   }
}
