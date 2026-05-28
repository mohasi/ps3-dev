using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;

namespace DebugBridgeClient
{
    // Trace tab: arms "module-trace-on <name> [withDeps]" on the bridge,
    // exercises the system, then "module-trace-off" pulls + parses the
    // capture file into the grid. Refresh re-fetches the last capture
    // without re-arming. Search highlights matching rows in place (no
    // filtering) and scrolls to the first match; Enter jumps to the next.
    public partial class TraceView : UserControl
    {
        private const string CapturePath = "/dev_hdd0/tmp/trace-capture.bin";

        private Ps3Connection ps3;
        private bool armed;
        private readonly List<TraceRow>  rows       = new List<TraceRow>();
        private readonly List<ParamRow>  paramRows  = new List<ParamRow>();

        public TraceView()
        {
            InitializeComponent();
            grid.ItemsSource          = rows;
            detailsParams.ItemsSource = paramRows;
            heatmap.CellClicked      += OnHeatmapCellClicked;
        }

        public void Attach(Ps3Connection connection) { ps3 = connection; }

        // ---- arm / disarm -------------------------------------------------

        private void OnStart(object sender, RoutedEventArgs e)
        {
            if (ps3 == null || !ps3.IsConnected) { SetStatus("not connected"); return; }
            string name = (moduleBox.Text ?? "").Trim();
            if (name.Length == 0) { SetStatus("enter a module name first"); return; }

            string cmd = "module-trace-on " + name + (depsCheck.IsChecked == true ? " withDeps" : "");
            SetStatus("arming " + name + "...");
            startButton.IsEnabled = false;
            // clear last run before the new one starts so the user never
            // sees stale rows next to a fresh "armed" status.
            rows.Clear();
            paramRows.Clear();
            grid.Items.Refresh();
            detailsParams.Items.Refresh();
            heatmap.Clear();
            System.Threading.ThreadPool.QueueUserWorkItem(delegate {
                Ps3Reply r = ps3.SendCommand(cmd);
                Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(delegate {
                    if (!r.Ok) {
                        SetStatus("arm failed: " + r.AsText().TrimEnd('\n'));
                        startButton.IsEnabled = true;
                        return;
                    }
                    armed = true;
                    stopButton.IsEnabled    = true;
                    moduleBox.IsEnabled     = false;
                    depsCheck.IsEnabled     = false;
                    refreshButton.IsEnabled = false;
                    SetStatus("armed: " + SummarizeArm(r.AsText()) + "  -  exercise then click Stop");
                }));
            });
        }

        private void OnStop(object sender, RoutedEventArgs e)
        {
            if (!armed) return;
            SetStatus("disarming...");
            stopButton.IsEnabled = false;
            System.Threading.ThreadPool.QueueUserWorkItem(delegate {
                Ps3Reply r = ps3.SendCommand("module-trace-off");
                Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(delegate {
                    armed = false;
                    moduleBox.IsEnabled     = true;
                    depsCheck.IsEnabled     = true;
                    startButton.IsEnabled   = true;
                    refreshButton.IsEnabled = true;
                    if (!r.Ok) {
                        SetStatus("disarm failed: " + r.AsText().TrimEnd('\n'));
                        return;
                    }
                    SetStatus("disarmed: " + r.AsText().TrimEnd('\n').Replace("\n", "  ") + "  -  loading capture...");
                    LoadCaptureAsync();
                }));
            });
        }

        private void OnRefresh(object sender, RoutedEventArgs e)
        {
            if (ps3 == null || !ps3.IsConnected) { SetStatus("not connected"); return; }
            if (armed) { SetStatus("stop the trace first"); return; }
            SetStatus("loading capture...");
            LoadCaptureAsync();
        }

        // ---- capture pull -------------------------------------------------

        private void LoadCaptureAsync()
        {
            System.Threading.ThreadPool.QueueUserWorkItem(delegate {
                Ps3Reply r = ps3.SendCommand("pull-file \"" + CapturePath + "\"");
                Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(delegate {
                    if (!r.Ok) {
                        SetStatus("pull-file failed: " + r.AsText().TrimEnd('\n'));
                        return;
                    }
                    Populate(r.Payload);
                }));
            });
        }

        private void Populate(byte[] bytes)
        {
            TraceCapture.Result parsed = TraceCapture.Parse(bytes);
            rows.Clear();
            if (parsed.Error != null) {
                SetStatus("parse failed: " + parsed.Error);
                grid.Items.Refresh();
                return;
            }
            for (int i = 0; i < parsed.Events.Count; i++)
                rows.Add(new TraceRow(parsed.Events[i]));
            grid.Items.Refresh();
            heatmap.SetEvents(rows);
            ApplyHighlight(false);

            TraceCapture.Summary s = parsed.Summary;
            string status = parsed.Events.Count + " events  (" + s.ManifestSlots + " slots in manifest";
            if (s.SlotsDropped > 0) status += ", " + s.SlotsDropped + " slots dropped at arm";
            if (s.RingDropped  > 0) status += ", " + s.RingDropped  + " ring drops";
            status += ")";
            SetStatus(status);
        }

        // Arm reply (cmd-trace.h): "hmod\t<name>\tslots=N\nhsum\tmods=M\tslots=N".
        // Show the hsum line for compactness; fall back to first line.
        private static string SummarizeArm(string text)
        {
            if (string.IsNullOrEmpty(text)) return "?";
            string[] lines = text.Split('\n');
            for (int i = 0; i < lines.Length; i++)
                if (lines[i].StartsWith("hsum")) return lines[i].Replace("\t", " ").TrimEnd();
            return lines[0].Replace("\t", " ").TrimEnd();
        }

        // ---- search / input ----------------------------------------------

        // mark every row IsMatch=true/false and (optionally) scroll to the
        // first match. typing always recomputes; only Enter advances to the
        // next match below the current view.
        private void ApplyHighlight(bool jumpToNext)
        {
            string q = (filterBox.Text ?? "").Trim();
            int matches = 0;
            int firstMatch = -1;
            int nextMatch  = -1;
            int startFrom  = jumpToNext ? FirstVisibleIndex() + 1 : 0;

            if (q.Length == 0) {
                for (int i = 0; i < rows.Count; i++) rows[i].IsMatch = false;
                return;
            }

            string lc = q.ToLowerInvariant();
            for (int i = 0; i < rows.Count; i++) {
                bool m = RowMatches(rows[i], lc);
                rows[i].IsMatch = m;
                if (!m) continue;
                matches++;
                if (firstMatch < 0) firstMatch = i;
                if (nextMatch  < 0 && i >= startFrom) nextMatch = i;
            }
            int target = (jumpToNext && nextMatch >= 0) ? nextMatch : firstMatch;
            if (target >= 0) {
                grid.ScrollIntoView(rows[target]);
                grid.SelectedIndex = target;
            }
            UpdateStatusMatches(matches);
        }

        // only match the columns the user actually sees in the grid
        // (Module, NID, Name); slot/r3/r4/r5 are details-pane content,
        // matching them would highlight rows for no visible reason.
        private static bool RowMatches(TraceRow row, string lc)
        {
            if (row.Module != null && row.Module.ToLowerInvariant().Contains(lc)) return true;
            if (row.Name   != null && row.Name  .ToLowerInvariant().Contains(lc)) return true;
            if (row.NidHex .Contains(lc)) return true;
            return false;
        }

        private int FirstVisibleIndex()
        {
            // best-effort: use the selected row if any, else 0. avoids
            // poking the virtualizing panel internals.
            return grid.SelectedIndex >= 0 ? grid.SelectedIndex : -1;
        }

        private void UpdateStatusMatches(int matches)
        {
            string q = (filterBox.Text ?? "").Trim();
            if (q.Length == 0) { /* leave status alone */ return; }
            SetStatus(matches + " match" + (matches == 1 ? "" : "es") + " for \"" + q + "\"");
        }

        private void OnFilterChanged(object sender, TextChangedEventArgs e)
        {
            filterPlaceholder.Visibility = string.IsNullOrEmpty(filterBox.Text)
                ? Visibility.Visible : Visibility.Collapsed;
            ApplyHighlight(false);
        }

        private void OnFilterKeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Enter) { ApplyHighlight(true); e.Handled = true; }
        }

        private void OnModuleTextChanged(object sender, TextChangedEventArgs e)
        {
            modulePlaceholder.Visibility = string.IsNullOrEmpty(moduleBox.Text)
                ? Visibility.Visible : Visibility.Collapsed;
        }

        private void OnModuleKeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Enter && startButton.IsEnabled) OnStart(sender, new RoutedEventArgs());
        }

        private void SetStatus(string msg) { statusText.Text = msg; }

        // clicking a heatmap cell selects the matching row in the grid
        // and scrolls it into view, so the viz doubles as a navigator.
        private void OnHeatmapCellClicked(int index)
        {
            if (index < 0 || index >= rows.Count) return;
            grid.SelectedIndex = index;
            grid.ScrollIntoView(rows[index]);
        }

        // ---- details pane -------------------------------------------------

        // re-render the bottom pane whenever the grid selection changes.
        // we resolve the prototype lazily here (per click) rather than at
        // load time so opening a capture stays cheap.
        private void OnRowSelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            paramRows.Clear();
            TraceRow row = grid.SelectedItem as TraceRow;
            if (row == null) { detailsParams.Items.Refresh(); return; }

            // no prototype known -> leave the pane empty rather than
            // inventing arg1/arg2/arg3 rows that imply r3/r4/r5 are real
            // args for nids that may take zero or take six.
            NidProtos.Proto proto = (row.Nid != 0) ? NidProtos.Resolve(row.Nid) : null;
            if (proto == null) { detailsParams.Items.Refresh(); return; }

            uint[] regs        = new[] { row.R3, row.R4, row.R5 };
            string[] regNames  = new[] { "r3", "r4", "r5" };
            for (int i = 0; i < proto.Args.Length; i++) {
                string typeName, paramName;
                SplitArg(proto.Args[i], out typeName, out paramName);
                string reg = (i < regNames.Length) ? regNames[i] : "-";
                string val = (i < regs.Length)
                    ? "0x" + regs[i].ToString("x8", CultureInfo.InvariantCulture)
                    : "-";
                paramRows.Add(new ParamRow { Reg = reg, Name = paramName, Type = typeName, Value = val });
            }
            detailsParams.Items.Refresh();
        }

        // last whitespace-separated token is the parameter name when the
        // arg looks like "<type...> <name>"; otherwise treat the whole
        // string as the type and leave the name blank.
        private static void SplitArg(string arg, out string typeName, out string paramName)
        {
            arg = (arg ?? "").Trim();
            if (arg.Length == 0) { typeName = ""; paramName = ""; return; }
            int sp = arg.LastIndexOf(' ');
            if (sp <= 0) { typeName = arg; paramName = ""; return; }
            typeName  = arg.Substring(0, sp).TrimEnd();
            paramName = arg.Substring(sp + 1);
        }
    }

    // grid row DTO: pre-formats hex columns once so the renderer doesn't
    // round-trip through a converter for every cell. IsMatch drives the
    // FlatRow DataTrigger that paints the highlight color.
    public class TraceRow : INotifyPropertyChanged
    {
        public int    Index   { get; private set; }
        public string Module  { get; private set; }
        public uint   Nid     { get; private set; }
        public string NidHex  { get; private set; }
        public string Name    { get; private set; }
        public string SlotHex { get; private set; }
        public uint   R3      { get; private set; }
        public uint   R4      { get; private set; }
        public uint   R5      { get; private set; }
        public string R3Hex   { get; private set; }
        public string R4Hex   { get; private set; }
        public string R5Hex   { get; private set; }

        private bool isMatch;
        public bool IsMatch {
            get { return isMatch; }
            set { if (isMatch == value) return; isMatch = value; Raise("IsMatch"); }
        }

        public event PropertyChangedEventHandler PropertyChanged;
        private void Raise(string name)
        {
            PropertyChangedEventHandler h = PropertyChanged;
            if (h != null) h(this, new PropertyChangedEventArgs(name));
        }

        public TraceRow(TraceCapture.Event ev)
        {
            Index   = ev.Index;
            Module  = ev.Module;
            Nid     = ev.Nid;
            NidHex  = "0x" + ev.Nid     .ToString("x8", CultureInfo.InvariantCulture);
            Name    = ev.Name;
            SlotHex = "0x" + ev.SlotAddr.ToString("x8", CultureInfo.InvariantCulture);
            R3 = ev.R3; R4 = ev.R4; R5 = ev.R5;
            R3Hex   = "0x" + ev.R3      .ToString("x8", CultureInfo.InvariantCulture);
            R4Hex   = "0x" + ev.R4      .ToString("x8", CultureInfo.InvariantCulture);
            R5Hex   = "0x" + ev.R5      .ToString("x8", CultureInfo.InvariantCulture);
        }
    }

    // details-pane row DTO.
    public class ParamRow
    {
        public string Reg   { get; set; }
        public string Type  { get; set; }
        public string Name  { get; set; }
        public string Value { get; set; }
    }
}

