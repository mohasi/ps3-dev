using System;
using System.Collections.Generic;
using System.Globalization;
using System.Threading;

namespace DebugBridgeClient
{
    // one process whose loaded modules we can introspect. processes are
    // discovered by the bridge's "process-list" command — vsh is always
    // present, an app process appears when an app has registered with
    // the bridge. transport is shared today (everything tunnels through
    // the bridge plugin in vsh); when app-side module enumeration is
    // wired in, only the transport delegate changes.
    public sealed class ModuleSource
    {
        public string DisplayName { get; private set; }
        public string Kind        { get; private set; }   // "vsh", "app", ...
        public string Status      { get; private set; }   // "live", "unavailable", ...
        public bool   CanListModules { get; private set; }

        private readonly Func<bool>             available;
        private readonly Func<string, Ps3Reply> send;

        public ModuleSource(string displayName, string kind, string status,
                            bool canListModules,
                            Func<bool> available, Func<string, Ps3Reply> send)
        {
            DisplayName    = displayName;
            Kind           = kind;
            Status         = status;
            CanListModules = canListModules;
            this.available = available;
            this.send      = send;
        }

        public bool IsAvailable { get { return available(); } }

        public void ListModules(Action<ModuleSummary[], string> done)
        {
            ThreadPool.QueueUserWorkItem(delegate {
                Ps3Reply r = send("module-list");
                if (!r.Ok) { done(null, r.AsText()); return; }
                done(ModuleParser.ParseList(r.AsText()), null);
            });
        }

        public void GetModuleInfo(string name, Action<ModuleDetails, string> done)
        {
            ThreadPool.QueueUserWorkItem(delegate {
                Ps3Reply r = send("module-info " + name);
                if (!r.Ok) { done(null, r.AsText()); return; }
                done(ModuleParser.ParseInfo(r.AsText()), null);
            });
        }
    }

    // top-level enumerator: asks the bridge "process-list" and turns the
    // result into ModuleSource instances. vsh is always backed by the
    // bridge transport; app processes are stubbed (no module enumeration
    // path yet) but still surfaced so the user can see them.
    public static class ProcessEnumerator
    {
        public static void ListProcesses(Ps3Connection ps3, Action<ModuleSource[], string> done)
        {
            ThreadPool.QueueUserWorkItem(delegate {
                if (!ps3.IsConnected) { done(null, "not connected"); return; }
                Ps3Reply r = ps3.SendCommand("process-list");
                if (!r.Ok) { done(null, r.AsText()); return; }
                done(Parse(ps3, r.AsText()), null);
            });
        }

        private static ModuleSource[] Parse(Ps3Connection ps3, string text)
        {
            var list = new List<ModuleSource>();
            if (string.IsNullOrEmpty(text)) return list.ToArray();
            foreach (string raw in text.Split('\n')) {
                string line = raw.TrimEnd('\r');
                if (line.Length == 0) continue;
                string[] f = line.Split('\t');
                if (f.Length < 2) continue;
                string kind   = f[0];
                string name   = f[1];
                string status = f.Length > 2 ? f[2] : "";
                list.Add(Build(ps3, kind, name, status));
            }
            return list.ToArray();
        }

        private static ModuleSource Build(Ps3Connection ps3, string kind, string name, string status)
        {
            if (kind == "vsh") {
                return new ModuleSource(name, kind, status, true,
                                        () => ps3.IsConnected,
                                        cmd => ps3.SendCommand(cmd));
            }
            // app (or anything else): visible in the tree, but no module
            // enumeration transport yet. CanListModules=false keeps the
            // refresh path from trying.
            return new ModuleSource(name, kind, status, false,
                                    () => false,
                                    cmd => Ps3Reply.Error("no transport"));
        }
    }

    public sealed class ModuleSummary
    {
        public string Id   { get; set; }
        public string Name { get; set; }
        public string File { get; set; }
    }

    public sealed class ModuleSegment
    {
        public uint   Index;
        public uint   Type;
        public ulong  Base;
        public ulong  FileSize;
        public ulong  MemSize;
    }

    public sealed class ModuleLib
    {
        public string Name;
        public uint   FuncCount;
        public uint   VarCount;
        public uint   TlsCount;
        public readonly List<ModuleFunc> Funcs = new List<ModuleFunc>();
    }

    public sealed class ModuleFunc
    {
        public uint Nid;
        public uint Addr;
    }

    public sealed class ModuleDetails
    {
        public string Id;
        public string Name;
        public string File;
        public uint   LibentAddr, LibentSize;
        public uint   LibstubAddr, LibstubSize;
        public readonly List<ModuleSegment> Segments = new List<ModuleSegment>();
        public readonly List<ModuleLib>     Exports  = new List<ModuleLib>();
        public readonly List<ModuleLib>     Imports  = new List<ModuleLib>();
    }

    // parses the text payload returned by the bridge's module-list and
    // module-info commands. format is tab-separated, one record per line:
    //   module-info: id, name, file, seg, libent, libstub, ent+ef, stub+sf
    //   module-list: <id>\t<name>\t<file>  (one line per module)
    public static class ModuleParser
    {
        public static ModuleSummary[] ParseList(string text)
        {
            if (string.IsNullOrEmpty(text)) return new ModuleSummary[0];
            string[] lines = text.Split('\n');
            var list = new List<ModuleSummary>(lines.Length);
            foreach (string raw in lines) {
                string line = raw.TrimEnd('\r');
                if (line.Length == 0) continue;
                string[] f = line.Split('\t');
                if (f.Length < 3) continue;
                list.Add(new ModuleSummary { Id = f[0], Name = f[1], File = f[2] });
            }
            return list.ToArray();
        }

        public static ModuleDetails ParseInfo(string text)
        {
            var d = new ModuleDetails();
            if (string.IsNullOrEmpty(text)) return d;
            ModuleLib currentExport = null;
            ModuleLib currentImport = null;
            foreach (string raw in text.Split('\n')) {
                string line = raw.TrimEnd('\r');
                if (line.Length == 0) continue;
                string[] f = line.Split('\t');
                switch (f[0]) {
                    case "id":      if (f.Length > 1) d.Id   = f[1]; break;
                    case "name":    if (f.Length > 1) d.Name = f[1]; break;
                    case "file":    if (f.Length > 1) d.File = f[1]; break;
                    case "libent":
                        if (f.Length >= 3) { d.LibentAddr = ParseHexU(f[1]); d.LibentSize = ParseU(f[2]); }
                        break;
                    case "libstub":
                        if (f.Length >= 3) { d.LibstubAddr = ParseHexU(f[1]); d.LibstubSize = ParseU(f[2]); }
                        break;
                    case "seg":
                        if (f.Length >= 6) d.Segments.Add(new ModuleSegment {
                            Index    = ParseU   (f[1]),
                            Type     = ParseHexU(f[2]),
                            Base     = ParseHexUL(f[3]),
                            FileSize = ParseHexUL(f[4]),
                            MemSize  = ParseHexUL(f[5])
                        });
                        break;
                    case "ent":
                        currentExport = NewLib(f);
                        currentImport = null;
                        d.Exports.Add(currentExport);
                        break;
                    case "stub":
                        currentImport = NewLib(f);
                        currentExport = null;
                        d.Imports.Add(currentImport);
                        break;
                    case "ef":
                        if (currentExport != null && f.Length >= 3)
                            currentExport.Funcs.Add(new ModuleFunc { Nid = ParseHexU(f[1]), Addr = ParseHexU(f[2]) });
                        break;
                    case "sf":
                        if (currentImport != null && f.Length >= 3)
                            currentImport.Funcs.Add(new ModuleFunc { Nid = ParseHexU(f[1]), Addr = ParseHexU(f[2]) });
                        break;
                }
            }
            return d;
        }

        private static ModuleLib NewLib(string[] f)
        {
            var lib = new ModuleLib();
            if (f.Length > 1) lib.Name = f[1];
            for (int i = 2; i < f.Length; i++) {
                int eq = f[i].IndexOf('=');
                if (eq < 0) continue;
                string k = f[i].Substring(0, eq);
                string v = f[i].Substring(eq + 1);
                if (k == "nfunc") lib.FuncCount = ParseU(v);
                else if (k == "nvar") lib.VarCount = ParseU(v);
                else if (k == "ntls") lib.TlsCount = ParseU(v);
            }
            return lib;
        }

        private static uint  ParseU   (string s) { uint  v; return uint .TryParse(s, out v) ? v : 0u; }
        private static uint  ParseHexU(string s) { uint  v; return uint .TryParse(StripHex(s), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v) ? v : 0u; }
        private static ulong ParseHexUL(string s){ ulong v; return ulong.TryParse(StripHex(s), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v) ? v : 0ul; }
        private static string StripHex(string s) { return (s != null && s.Length > 2 && (s[1] == 'x' || s[1] == 'X')) ? s.Substring(2) : s; }
    }
}
