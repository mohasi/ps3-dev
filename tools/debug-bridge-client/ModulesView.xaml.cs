using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;

namespace DebugBridgeClient
{
    // Modules tab: a TreeView built from the bridge's process-list.
    // Top-level nodes are processes (vsh, app, ...), children are
    // modules, grandchildren are segments / exports / imports loaded
    // lazily when the module is expanded. Refresh re-enumerates the
    // process list and then each process's module list.
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

        private void OnProcessesDone(ModuleSource[] procs, string err)
        {
            if (err != null || procs == null) { statusText.Text = "process-list failed: " + (err ?? "no data"); return; }
            foreach (ModuleSource s in procs) tree.Items.Add(BuildProcessNode(s));
            int pending = 0;
            foreach (ModuleSource s in procs) if (s.CanListModules && s.IsAvailable) pending++;
            if (pending == 0) { statusText.Text = procs.Length + " processes"; return; }
            statusText.Text = "loading modules...";
            var ctx = new RefreshCtx { Pending = pending };
            foreach (ModuleSource s in procs) {
                if (!s.CanListModules || !s.IsAvailable) continue;
                ModuleSource captured = s;
                captured.ListModules((mods, mErr) =>
                    Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() =>
                        OnSourceDone(captured, mods, mErr, ctx))));
            }
        }

        private void OnSourceDone(ModuleSource source, ModuleSummary[] mods, string err, RefreshCtx ctx)
        {
            TreeViewItem node = FindProcessNode(source);
            if (node == null) return;
            if (err != null || mods == null) {
                node.Header = source.DisplayName + "  [" + (err ?? "no data") + "]";
                ctx.Failed++;
            } else {
                node.Header = source.DisplayName + "  (" + mods.Length + " modules)";
                node.IsExpanded = true;
                foreach (ModuleSummary m in mods) node.Items.Add(BuildModuleNode(source, m));
                ctx.Loaded += mods.Length;
            }
            ctx.Pending--;
            if (ctx.Pending == 0)
                statusText.Text = "Loaded " + ctx.Loaded + " modules" + (ctx.Failed > 0 ? " (" + ctx.Failed + " sources failed)" : "");
        }

        private sealed class RefreshCtx { public int Pending, Loaded, Failed; }

        private TreeViewItem FindProcessNode(ModuleSource source)
        {
            foreach (object o in tree.Items) {
                TreeViewItem t = o as TreeViewItem;
                if (t != null && t.Tag == source) return t;
            }
            return null;
        }

        private TreeViewItem BuildProcessNode(ModuleSource source)
        {
            string suffix = "  [" + source.Kind + (string.IsNullOrEmpty(source.Status) ? "" : ", " + source.Status) + "]";
            if (!source.CanListModules) suffix += "  (no module access)";
            return new TreeViewItem { Header = source.DisplayName + suffix, Tag = source };
        }

        // module nodes start with a single placeholder child so the
        // expander chevron shows up; when the user expands, the placeholder
        // is replaced with real segments/exports/imports fetched on demand.
        private TreeViewItem BuildModuleNode(ModuleSource source, ModuleSummary m)
        {
            var node = new TreeViewItem {
                Header     = m.Name + "    [" + m.Id + "]    " + m.File,
                Tag        = new ModuleNodeState { Source = source, Summary = m, Loaded = false }
            };
            node.Items.Add(new TreeViewItem { Header = "(loading...)" });
            node.Expanded += OnModuleExpanded;
            return node;
        }

        private void OnModuleExpanded(object sender, RoutedEventArgs e)
        {
            TreeViewItem node = sender as TreeViewItem;
            if (node == null) return;
            ModuleNodeState st = node.Tag as ModuleNodeState;
            if (st == null || st.Loaded) return;
            st.Loaded = true;
            st.Source.GetModuleInfo(st.Summary.Name, (details, err) =>
                Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(() => {
                    node.Items.Clear();
                    if (err != null || details == null) {
                        node.Items.Add(Leaf("error: " + (err ?? "no data")));
                        return;
                    }
                    PopulateDetails(node, details);
                })));
        }

        private static void PopulateDetails(TreeViewItem node, ModuleDetails d)
        {
            // segments
            TreeViewItem segs = new TreeViewItem { Header = "segments (" + d.Segments.Count + ")" };
            foreach (ModuleSegment s in d.Segments) {
                segs.Items.Add(Leaf(string.Format(
                    "seg {0}  type=0x{1:x}  base=0x{2:x}  filesz=0x{3:x}  memsz=0x{4:x}",
                    s.Index, s.Type, s.Base, s.FileSize, s.MemSize)));
            }
            node.Items.Add(segs);

            // linkage tables (optional context)
            TreeViewItem link = new TreeViewItem { Header = "linkage" };
            link.Items.Add(Leaf(string.Format("libent  addr=0x{0:x8}  size={1}", d.LibentAddr,  d.LibentSize)));
            link.Items.Add(Leaf(string.Format("libstub addr=0x{0:x8}  size={1}", d.LibstubAddr, d.LibstubSize)));
            node.Items.Add(link);

            node.Items.Add(BuildLibsNode("exports", d.Exports));
            node.Items.Add(BuildLibsNode("imports", d.Imports));
        }

        private static TreeViewItem BuildLibsNode(string label, List<ModuleLib> libs)
        {
            int totalFuncs = 0;
            foreach (ModuleLib l in libs) totalFuncs += l.Funcs.Count;
            TreeViewItem root = new TreeViewItem {
                Header = label + " (" + libs.Count + " libs, " + totalFuncs + " funcs)"
            };
            foreach (ModuleLib lib in libs) {
                TreeViewItem libNode = new TreeViewItem {
                    Header = (lib.Name ?? "<anon>") +
                             "    nfunc=" + lib.FuncCount +
                             "  nvar="    + lib.VarCount +
                             "  ntls="    + lib.TlsCount
                };
                foreach (ModuleFunc fn in lib.Funcs) {
                    libNode.Items.Add(Leaf(string.Format("0x{0:x8}  addr=0x{1:x8}", fn.Nid, fn.Addr)));
                }
                root.Items.Add(libNode);
            }
            return root;
        }

        private static TreeViewItem Leaf(string text)
        {
            return new TreeViewItem { Header = text };
        }

        private sealed class ModuleNodeState
        {
            public ModuleSource  Source;
            public ModuleSummary Summary;
            public bool          Loaded;
        }
    }
}
