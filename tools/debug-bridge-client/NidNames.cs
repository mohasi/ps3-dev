using System.Collections.Generic;
using System.IO;

namespace DebugBridgeClient
{
    // Lazy NID→name resolver. Two sources, in priority order:
    //   1. nid_names_local.json - hand-curated entries for symbols our
    //      own code uses (e.g. VSH-private exports like vshtask_A02D46E7).
    //      Marked as source "self" so the UI can label them as ours, not
    //      as upstream-known names.
    //   2. nid_names.json       - the bulk upstream NID database, tagged
    //      as source "sdk".
    // Both files share the same flat-object shape ("0xNNNNNNNN": "name",
    // one pair per line). The bulk file is ~700 KB / ~30k entries; we
    // only load on first lookup so the UI stays snappy if the user
    // never opens an exports/imports node.
    public static class NidNames
    {
        public struct Entry { public string Name; public string Source; }

        private static Dictionary<uint, string> map;        // sdk
        private static Dictionary<uint, string> mapLocal;   // self
        private static readonly object loadLock = new object();

        public static Entry Resolve(uint nid)
        {
            EnsureLoaded();
            string name;
            if (mapLocal != null && mapLocal.TryGetValue(nid, out name))
                return new Entry { Name = name, Source = "self" };
            if (map != null && map.TryGetValue(nid, out name))
                return new Entry { Name = name, Source = "sdk" };
            return new Entry { Name = null, Source = null };
        }

        private static void EnsureLoaded()
        {
            if (map != null) return;
            lock (loadLock) {
                if (map != null) return;
                string localPath = NidJson.FindFile("nid_names_local.json");
                mapLocal = localPath != null ? Parse(localPath) : new Dictionary<uint, string>();
                string bulkPath  = NidJson.FindFile("nid_names.json");
                map      = bulkPath  != null ? Parse(bulkPath)  : new Dictionary<uint, string>();
            }
        }

        // Minimal parser for the exact shape the file uses:
        //   {
        //     "0xNNNNNNNN": "name",
        //     ...
        //   }
        // One pair per line, no nested objects, no escaped quotes in names.
        // Fast (single pass, no regex) and dependency-free.
        private static Dictionary<uint, string> Parse(string path)
        {
            var result = new Dictionary<uint, string>(32768);
            try {
                using (var reader = new StreamReader(path)) {
                    string line;
                    while ((line = reader.ReadLine()) != null) {
                        int q1 = line.IndexOf('"');
                        if (q1 < 0) continue;
                        int q2 = line.IndexOf('"', q1 + 1);
                        if (q2 < 0) continue;
                        int q3 = line.IndexOf('"', q2 + 1);
                        if (q3 < 0) continue;
                        int q4 = line.IndexOf('"', q3 + 1);
                        if (q4 < 0) continue;
                        string key  = line.Substring(q1 + 1, q2 - q1 - 1);
                        string name = line.Substring(q3 + 1, q4 - q3 - 1);
                        uint nid;
                        if (NidJson.TryParseKey(key, out nid)) result[nid] = name;
                    }
                }
            } catch {
                // file unreadable: leave map empty, resolve() returns null,
                // tree falls back to plain NIDs.
            }
            return result;
        }
    }
}
