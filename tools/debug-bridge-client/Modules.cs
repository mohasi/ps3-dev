using System;
using System.Collections.Generic;
using System.Globalization;
using System.Threading;

namespace DebugBridgeClient
{
    // one process the bridge can introspect. processes are discovered
    // by "process-list"; details (pid, sdk, loaded modules) come from
    // "process-info <name>". on cex the only process the bridge can
    // see is vsh itself - see git history for the read-only probe.
    public sealed class ProcessSource
    {
        public string DisplayName { get; private set; }
        public string Status      { get; private set; }   // "live", ...

        private readonly Func<bool>             available;
        private readonly Func<string, Ps3Reply> send;

        public ProcessSource(string displayName, string status,
                             Func<bool> available, Func<string, Ps3Reply> send)
        {
            DisplayName    = displayName;
            Status         = status;
            this.available = available;
            this.send      = send;
        }

        public bool IsAvailable { get { return available(); } }

        public void GetProcessInfo(Action<ProcessDetails, string> done)
        {
            ThreadPool.QueueUserWorkItem(delegate {
                Ps3Reply r = send("process-info " + DisplayName);
                if (!r.Ok) { done(null, r.AsText()); return; }
                done(ProcessParser.ParseInfo(r.AsText()), null);
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

    // top-level enumerator: asks the bridge "process-list" and turns
    // each row into a ProcessSource. wire format is "<name>\t<status>\n".
    public static class ProcessEnumerator
    {
        public static void ListProcesses(Ps3Connection ps3, Action<ProcessSource[], string> done)
        {
            ThreadPool.QueueUserWorkItem(delegate {
                if (!ps3.IsConnected) { done(null, "not connected"); return; }
                Ps3Reply r = ps3.SendCommand("process-list");
                if (!r.Ok) { done(null, r.AsText()); return; }
                done(Parse(ps3, r.AsText()), null);
            });
        }

        private static ProcessSource[] Parse(Ps3Connection ps3, string text)
        {
            var list = new List<ProcessSource>();
            if (string.IsNullOrEmpty(text)) return list.ToArray();
            foreach (string raw in text.Split('\n')) {
                string line = raw.TrimEnd('\r');
                if (line.Length == 0) continue;
                string[] f = line.Split('\t');
                if (f.Length < 1) continue;
                string name   = f[0];
                string status = f.Length > 1 ? f[1] : "";
                list.Add(new ProcessSource(name, status,
                                           () => ps3.IsConnected,
                                           cmd => ps3.SendCommand(cmd)));
            }
            return list.ToArray();
        }
    }

    public sealed class ProcessDetails
    {
        public ulong  Pid;
        public string Name;
        public uint   SdkVersion;
        public readonly List<ModuleSummary> Modules = new List<ModuleSummary>();
        // process-owned linkage recovered from the main executable's
        // live ELF mapping (no sce_prx_param on a main self). Segments
        // are PT_LOAD entries; Exports/Imports mirror the per-PRX
        // ModuleDetails shape so the UI can render them the same way.
        public readonly List<ModuleSegment> Segments = new List<ModuleSegment>();
        public readonly List<ModuleLib>     Exports  = new List<ModuleLib>();
        public readonly List<ModuleLib>     Imports  = new List<ModuleLib>();
    }

    // parses "process-info" output. format is tab-separated rows:
    //   pid    0x<hex>
    //   name   <text>
    //   sdk    0x<hex>
    //   mod    <name>\t<file>            (one per loaded prx)
    //   pseg   <i>\t<type>\t<base>\t<filesz>\t<memsz>   (main-exe segs)
    //   pent   <libname>\tnfunc=..\tnvar=..\tntls=..    (main-exe export lib)
    //     pef  <nid>                                    (export func)
    //     pev  <nid>                                    (export var)
    //   pstub  <libname>\tnfunc=..\tnvar=..\tntls=..    (main-exe import lib)
    //     psf  <nid>                                    (import func)
    //     psv  <nid>                                    (import var)
    public static class ProcessParser
    {
        public static ProcessDetails ParseInfo(string text)
        {
            var d = new ProcessDetails();
            if (string.IsNullOrEmpty(text)) return d;
            ModuleLib currentExport = null;
            ModuleLib currentImport = null;
            foreach (string raw in text.Split('\n')) {
                string line = raw.TrimEnd('\r');
                if (line.Length == 0) continue;
                string[] f = line.Split('\t');
                switch (f[0]) {
                    case "pid":  if (f.Length > 1) d.Pid        = ParseHelpers.HexUL(f[1]); break;
                    case "name": if (f.Length > 1) d.Name       = f[1];                    break;
                    case "sdk":  if (f.Length > 1) d.SdkVersion = ParseHelpers.HexU (f[1]); break;
                    case "mod":
                        if (f.Length >= 3)
                            d.Modules.Add(new ModuleSummary { Name = f[1], File = f[2] });
                        break;
                    case "pseg":  ParseHelpers.AddSegment(d.Segments, f); break;
                    case "pent":  currentExport = ParseHelpers.NewLib(f); currentImport = null; d.Exports.Add(currentExport); break;
                    case "pstub": currentImport = ParseHelpers.NewLib(f); currentExport = null; d.Imports.Add(currentImport); break;
                    case "pef":   ParseHelpers.AddNid(currentExport, f, false); break;
                    case "pev":   ParseHelpers.AddNid(currentExport, f, true);  break;
                    case "psf":   ParseHelpers.AddNid(currentImport, f, false); break;
                    case "psv":   ParseHelpers.AddNid(currentImport, f, true);  break;
                }
            }
            return d;
        }
    }

    public sealed class ModuleSummary
    {
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
        public readonly List<ModuleFunc> Vars  = new List<ModuleFunc>();
    }

    public sealed class ModuleFunc
    {
        public uint Nid;
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
    //   module-info: id, name, file, seg, libent, libstub,
    //                ent+ef+ev (exports), stub+sf+sv (imports)
    //   module-list: <name>\t<file>  (one line per module)
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
                if (f.Length < 2) continue;
                list.Add(new ModuleSummary { Name = f[0], File = f[1] });
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
                    case "libent":  if (f.Length >= 3) { d.LibentAddr  = ParseHelpers.HexU(f[1]); d.LibentSize  = ParseHelpers.U(f[2]); } break;
                    case "libstub": if (f.Length >= 3) { d.LibstubAddr = ParseHelpers.HexU(f[1]); d.LibstubSize = ParseHelpers.U(f[2]); } break;
                    case "seg":     ParseHelpers.AddSegment(d.Segments, f); break;
                    case "ent":     currentExport = ParseHelpers.NewLib(f); currentImport = null; d.Exports.Add(currentExport); break;
                    case "stub":    currentImport = ParseHelpers.NewLib(f); currentExport = null; d.Imports.Add(currentImport); break;
                    case "ef":      ParseHelpers.AddNid(currentExport, f, false); break;
                    case "ev":      ParseHelpers.AddNid(currentExport, f, true);  break;
                    case "sf":      ParseHelpers.AddNid(currentImport, f, false); break;
                    case "sv":      ParseHelpers.AddNid(currentImport, f, true);  break;
                }
            }
            return d;
        }
    }

    // shared row parsers. tag prefixes differ between process-info
    // (pseg/pent/pef/...) and module-info (seg/ent/ef/...) but the
    // column layout is identical, so the body of each handler is the
    // same. lib/segment building lives here so both callers stay tiny.
    internal static class ParseHelpers
    {
        public static ModuleLib NewLib(string[] f)
        {
            var lib = new ModuleLib();
            if (f.Length > 1) lib.Name = f[1];
            for (int i = 2; i < f.Length; i++) {
                int eq = f[i].IndexOf('=');
                if (eq < 0) continue;
                string k = f[i].Substring(0, eq);
                string v = f[i].Substring(eq + 1);
                if      (k == "nfunc") lib.FuncCount = U(v);
                else if (k == "nvar")  lib.VarCount  = U(v);
                else if (k == "ntls")  lib.TlsCount  = U(v);
            }
            return lib;
        }

        public static void AddSegment(List<ModuleSegment> dst, string[] f)
        {
            if (f.Length < 6) return;
            dst.Add(new ModuleSegment {
                Index    = U   (f[1]),
                Type     = HexU(f[2]),
                Base     = HexUL(f[3]),
                FileSize = HexUL(f[4]),
                MemSize  = HexUL(f[5])
            });
        }

        public static void AddNid(ModuleLib target, string[] f, bool isVar)
        {
            if (target == null || f.Length < 2) return;
            var entry = new ModuleFunc { Nid = HexU(f[1]) };
            (isVar ? target.Vars : target.Funcs).Add(entry);
        }

        public static uint  U    (string s) { uint  v; return uint .TryParse(s, out v) ? v : 0u; }
        public static uint  HexU (string s) { uint  v; return uint .TryParse(StripHex(s), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v) ? v : 0u; }
        public static ulong HexUL(string s) { ulong v; return ulong.TryParse(StripHex(s), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out v) ? v : 0ul; }
        private static string StripHex(string s) { return (s != null && s.Length > 2 && (s[1] == 'x' || s[1] == 'X')) ? s.Substring(2) : s; }
    }
}
