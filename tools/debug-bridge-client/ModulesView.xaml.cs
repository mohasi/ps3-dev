using System;
using System.Collections.Generic;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;

namespace DebugBridgeClient
{
    // Modules tab: a TreeView built from the bridge's process-list.
    // Top-level nodes are processes (vsh on cex). Refresh only fetches
    // the process list — expanding a process node issues "process-info"
    // to fetch pid/sdk + loaded modules. Expanding a module node issues
    // "module-info" to fetch segments + imports/exports. Everything
    // below process level is lazy: the user pays only for what they
    // open.
    public partial class ModulesView : UserControl
    {
        private Ps3Connection ps3;

        public ModulesView() { InitializeComponent(); }

        public void Attach(Ps3Connection connection) { ps3 = connection; }

        private void OnRefresh(object sender, RoutedEventArgs e)
        {
            tree.Items.Clear();
            if (ps3 == null || !ps3.IsConnected) { statusText.Text = "not connected"; return; }
            statusText.Text = "loading processes...";
            ProcessEnumerator.ListProcesses(ps3, (procs, err) =>
                Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
                    OnProcessesDone(procs, err))));
        }

        private void OnProcessesDone(ProcessSource[] procs, string err)
        {
            if (err != null || procs == null) { statusText.Text = "process-list failed: " + (err ?? "no data"); return; }
            foreach (ProcessSource s in procs) tree.Items.Add(BuildProcessNode(s));
            statusText.Text = procs.Length + " process" + (procs.Length == 1 ? "" : "es");
        }

        // process nodes start with a placeholder child so the expander
        // shows up; on first expand, the placeholder is replaced with
        // pid/sdk leaves + one TreeViewItem per loaded module.
        private TreeViewItem BuildProcessNode(ProcessSource source)
        {
            string suffix = string.IsNullOrEmpty(source.Status) ? "" : "  [" + source.Status + "]";
            var node = new TreeViewItem {
                Header = source.DisplayName + suffix,
                Tag    = new ProcessNodeState { Source = source, Loaded = false }
            };
            node.Items.Add(new TreeViewItem { Header = "(loading...)" });
            node.Expanded += OnProcessExpanded;
            return node;
        }

        private void OnProcessExpanded(object sender, RoutedEventArgs e)
        {
            TreeViewItem node = sender as TreeViewItem;
            if (node == null) return;
            ProcessNodeState st = node.Tag as ProcessNodeState;
            if (st == null || st.Loaded) return;
            st.Loaded = true;
            st.Source.GetProcessInfo((details, err) =>
                Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() => {
                    node.Items.Clear();
                    if (err != null || details == null) {
                        node.Items.Add(Leaf("error: " + (err ?? "no data")));
                        st.Loaded = false; // allow retry on next expand
                        return;
                    }
                    PopulateProcess(node, st.Source, details);
                })));
        }

        private static void PopulateProcess(TreeViewItem node, ProcessSource source, ProcessDetails d)
        {
            node.Items.Add(Leaf(string.Format("pid 0x{0:x}", d.Pid)));
            node.Items.Add(Leaf("sdk " + FormatSdkVersion(d.SdkVersion)));
            if (d.Segments.Count > 0) {
                TreeViewItem segs = new TreeViewItem { Header = "segments (" + d.Segments.Count + ")" };
                foreach (ModuleSegment s in d.Segments) segs.Items.Add(Leaf(FormatSegment(s)));
                node.Items.Add(segs);
            }
            if (d.Imports.Count > 0) node.Items.Add(BuildLibsNode("imports", d.Imports));
            if (d.Exports.Count > 0) node.Items.Add(BuildLibsNode("exports", d.Exports));
            TreeViewItem mods = new TreeViewItem {
                Header     = "modules (" + d.Modules.Count + ")",
                IsExpanded = true
            };
            // sort by name so the list is scannable; bridge emits load order.
            List<ModuleSummary> sorted = new List<ModuleSummary>(d.Modules);
            sorted.Sort(delegate(ModuleSummary a, ModuleSummary b) {
                return string.Compare(a.Name, b.Name, System.StringComparison.OrdinalIgnoreCase);
            });
            foreach (ModuleSummary m in sorted) mods.Items.Add(BuildModuleNode(source, m));
            node.Items.Add(mods);
        }

        // module nodes start with a single placeholder child so the
        // expander chevron shows up; when the user expands, the placeholder
        // is replaced with real segments/exports/imports fetched on demand.
        private static TreeViewItem BuildModuleNode(ProcessSource source, ModuleSummary m)
        {
            var node = new TreeViewItem {
                Header = m.Name,
                Tag    = new ModuleNodeState { Source = source, Summary = m, Loaded = false }
            };
            node.Items.Add(new TreeViewItem { Header = "(loading...)" });
            node.Expanded += OnModuleExpanded;
            return node;
        }

        private static void OnModuleExpanded(object sender, RoutedEventArgs e)
        {
            TreeViewItem node = sender as TreeViewItem;
            if (node == null) return;
            ModuleNodeState st = node.Tag as ModuleNodeState;
            if (st == null || st.Loaded) return;
            st.Loaded = true;
            st.Source.GetModuleInfo(st.Summary.Name, (details, err) =>
                node.Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() => {
                    node.Items.Clear();
                    if (err != null || details == null) {
                        node.Items.Add(Leaf("error: " + (err ?? "no data")));
                        st.Loaded = false;
                        return;
                    }
                    PopulateModule(node, st.Summary, details);
                })));
        }

        private static void PopulateModule(TreeViewItem node, ModuleSummary summary, ModuleDetails d)
        {
            node.Items.Add(Leaf("path  " + summary.File));

            TreeViewItem segs = new TreeViewItem { Header = "segments (" + d.Segments.Count + ")" };
            foreach (ModuleSegment s in d.Segments) segs.Items.Add(Leaf(FormatSegment(s)));
            node.Items.Add(segs);

            TreeViewItem link = new TreeViewItem { Header = "linkage" };
            link.Items.Add(Leaf(string.Format("libent  addr=0x{0:x8}  size={1}", d.LibentAddr,  d.LibentSize)));
            link.Items.Add(Leaf(string.Format("libstub addr=0x{0:x8}  size={1}", d.LibstubAddr, d.LibstubSize)));
            node.Items.Add(link);

            node.Items.Add(BuildLibsNode("exports", d.Exports));
            node.Items.Add(BuildLibsNode("imports", d.Imports));
        }

        private static TreeViewItem BuildLibsNode(string label, List<ModuleLib> libs)
        {
            int totalFuncs = 0, totalVars = 0;
            foreach (ModuleLib l in libs) { totalFuncs += l.Funcs.Count; totalVars += l.Vars.Count; }
            string header = label + "  (" + libs.Count + " libs, " + totalFuncs + " funcs";
            if (totalVars > 0) header += ", " + totalVars + " vars";
            header += ")";
            TreeViewItem root = new TreeViewItem { Header = header };
            foreach (ModuleLib lib in libs) {
                TreeViewItem libNode = new TreeViewItem {
                    Header = LibDisplayName(lib) + "  (" + FormatLibCounts(lib) + ")"
                };
                foreach (ModuleFunc fn in lib.Funcs)
                    libNode.Items.Add(Leaf(FormatNid(fn.Nid, false)));
                foreach (ModuleFunc v in lib.Vars)
                    libNode.Items.Add(Leaf(FormatNid(v.Nid, true)));
                root.Items.Add(libNode);
            }
            return root;
        }

        // "0x........  name (var) [self]" - var marker only for variables,
        // source marker only for hand-labelled entries in nid_names_local.json
        // (upstream sdk names are common and don't need a tag).
        private static string FormatNid(uint nid, bool isVar)
        {
            NidNames.Entry e = NidNames.Resolve(nid);
            string s = string.Format("0x{0:x8}  {1}", nid, e.Name ?? "?");
            if (isVar)            s += "  (var)";
            if (e.Source == "self") s += "  [self]";
            return s;
        }

        // "4.93" from packed sdk version 0x00MMmm00 (e.g. 0x00493000 -> "4.93":
        // the version digits live in nibbles 5..2, with minor as a two-digit
        // BCD-ish value). Falls back to raw hex if the field looks unrecognized.
        private static string FormatSdkVersion(uint v)
        {
            uint major = (v >> 20) & 0xfu;
            uint minor = (v >> 12) & 0xffu;
            if (major == 0 && minor == 0) return string.Format("0x{0:x}", v);
            return string.Format("{0:x}.{1:x2}", major, minor);
        }

        private static string FormatSegment(ModuleSegment s)
        {
            return string.Format("seg {0}  type=0x{1:x}  base=0x{2:x}  filesz=0x{3:x}  memsz=0x{4:x}",
                                 s.Index, s.Type, s.Base, s.FileSize, s.MemSize);
        }

        // PRX always have one anonymous export group for module_start /
        // module_stop / module_info; its libname pointer is 0 so the bridge
        // emits an empty string. Show it as <module> so it's obvious what
        // those entries belong to, not just a blank header.
        private static string LibDisplayName(ModuleLib lib)
        {
            if (string.IsNullOrEmpty(lib.Name)) return "<module>";
            return lib.Name;
        }

        // Only show non-zero columns. tls is almost always zero on PS3
        // PRX, and the long "funcs=N vars=0 tls=0" reads like noise.
        private static string FormatLibCounts(ModuleLib lib)
        {
            string s = lib.FuncCount + " funcs";
            if (lib.VarCount > 0) s += ", " + lib.VarCount + " vars";
            if (lib.TlsCount > 0) s += ", " + lib.TlsCount + " tls";
            return s;
        }

        private static TreeViewItem Leaf(string text)
        {
            return new TreeViewItem { Header = text };
        }

        // Ctrl+C copies just the selected node's header (the common case:
        // grab one NID line). Shift+Ctrl+C copies the subtree.
        private void OnTreeKeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key != Key.C) return;
            ModifierKeys mods = Keyboard.Modifiers;
            if ((mods & ModifierKeys.Control) == 0) return;
            bool subtree = (mods & ModifierKeys.Shift) != 0;
            CopySelected(subtree);
            e.Handled = true;
        }

        private void OnCopyNode    (object sender, RoutedEventArgs e) { CopySelected(false); }
        private void OnCopySubtree (object sender, RoutedEventArgs e) { CopySelected(true);  }

        private void CopySelected(bool subtree)
        {
            TreeViewItem node = tree.SelectedItem as TreeViewItem;
            if (node == null) return;
            string text = subtree ? RenderSubtree(node, 0) : HeaderText(node);
            try { Clipboard.SetText(text); } catch { /* clipboard busy */ }
        }

        private static string HeaderText(TreeViewItem node)
        {
            return node.Header == null ? "" : node.Header.ToString();
        }

        private static string RenderSubtree(TreeViewItem node, int depth)
        {
            var sb = new StringBuilder();
            sb.Append(' ', depth * 2);
            sb.AppendLine(HeaderText(node));
            foreach (object child in node.Items) {
                TreeViewItem c = child as TreeViewItem;
                if (c != null) sb.Append(RenderSubtree(c, depth + 1));
            }
            return sb.ToString();
        }

        private sealed class ProcessNodeState
        {
            public ProcessSource Source;
            public bool          Loaded;
        }

        private sealed class ModuleNodeState
        {
            public ProcessSource Source;
            public ModuleSummary Summary;
            public bool          Loaded;
        }
    }
}
