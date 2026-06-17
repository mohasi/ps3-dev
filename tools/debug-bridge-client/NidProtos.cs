using System.Collections.Generic;
using System.IO;

namespace DebugBridgeClient
{
   // Lazy NID -> prototype resolver. Reads nid_protos.json, which is
   // produced by dev/tools/nid-protos-from-headers.ps1 by scanning
   // SDK headers and joining against nid_names.json. Shape:
   //
   //   {
   //     "0x718BF5F8": { "ret": "CellFsErrno",
   //                     "args": ["const char* path", "int flags",
   //                              "int* fd", "const void* arg",
   //                              "uint64_t size"] },
   //     ...
   //   }
   //
   // One entry per line, same flat-object style as nid_names.json, so a
   // tiny line-oriented parser keeps us dependency-free.
   public static class NidProtos
   {
      public class Proto
      {
         public string   Ret;
         public string[] Args;   // each entry is "<type> <name>" or just "<type>"
      }

      private static Dictionary<uint, Proto> map;
      private static readonly object loadLock = new object();

      public static Proto Resolve(uint nid)
      {
         EnsureLoaded();
         Proto p;
         if (map != null && map.TryGetValue(nid, out p)) return p;
         return null;
      }

      private static void EnsureLoaded()
      {
         if (map != null) return;
         lock (loadLock) {
            if (map != null) return;
            string path = NidJson.FindFile("nid_protos.json");
            map = path != null ? Parse(path) : new Dictionary<uint, Proto>();
         }
      }

      // Parse one entry per line. We don't need a general JSON parser:
      // the writer (nid-protos-from-headers.ps1) always emits exactly:
      //   "0xNNNNNNNN": { "ret": "<ret>", "args": [<csv strings>] },
      // with no embedded newlines and no escaped quotes in types.
      private static Dictionary<uint, Proto> Parse(string path)
      {
         var result = new Dictionary<uint, Proto>(8192);
         try {
            using (var reader = new StreamReader(path)) {
               string line;
               while ((line = reader.ReadLine()) != null) {
                  Proto p; uint nid;
                  if (TryParseLine(line, out nid, out p)) result[nid] = p;
               }
            }
         } catch {
            // bad/missing file: leave map empty, callers fall back to NID.
         }
         return result;
      }

      private static bool TryParseLine(string line, out uint nid, out Proto proto)
      {
         nid = 0; proto = null;

         // key: "0xNNNNNNNN"
         int q1 = line.IndexOf('"'); if (q1 < 0) return false;
         int q2 = line.IndexOf('"', q1 + 1); if (q2 < 0) return false;
         string key = line.Substring(q1 + 1, q2 - q1 - 1);
         if (!NidJson.TryParseKey(key, out nid)) return false;

         // ret: "ret": "<value>"
         int retTag = line.IndexOf("\"ret\"", q2);
         if (retTag < 0) return false;
         int rq1 = line.IndexOf('"', retTag + 5); if (rq1 < 0) return false;
         rq1     = line.IndexOf('"', rq1 + 1);    if (rq1 < 0) return false;   // skip colon+space, land on value-open
         int rq2 = line.IndexOf('"', rq1 + 1);    if (rq2 < 0) return false;
         string ret = line.Substring(rq1 + 1, rq2 - rq1 - 1);

         // args: "args": [ "a", "b", ... ]
         int argsTag = line.IndexOf("\"args\"", rq2);
         if (argsTag < 0) return false;
         int lb = line.IndexOf('[', argsTag); if (lb < 0) return false;
         int rb = line.IndexOf(']', lb);      if (rb < 0) return false;
         var args = new List<string>();
         int i = lb + 1;
         while (i < rb) {
            int aq1 = line.IndexOf('"', i, rb - i); if (aq1 < 0) break;
            int aq2 = line.IndexOf('"', aq1 + 1, rb - aq1 - 1); if (aq2 < 0) break;
            args.Add(line.Substring(aq1 + 1, aq2 - aq1 - 1));
            i = aq2 + 1;
         }
         proto = new Proto { Ret = ret, Args = args.ToArray() };
         return true;
      }
   }
}
