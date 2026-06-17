using System;
using System.Collections.Generic;
using System.Text;

namespace DebugBridgeClient
{
   // Pure parser for the bridge's trace capture format (cmd-trace.h, v3).
   // Layout:
   //   [0..4)   magic "TRAC"
   //   [4..8)   version u32 big-endian (3)
   //   [8..12)  manifestOffset u32 big-endian
   //   [12..16) reserved
   //   [16..manifestOffset)  event stream: 16 bytes BE { slotAddr, r3, r4, r5 }
   //   [manifestOffset..]    ascii:
   //       "\n==MANIFEST==\n"
   //       "slot 0x<addr>\t<module>\t0x<nid>\n" (one per armed slot)
   //       "==SUMMARY==\nslots\t...\nevents\t...\n"
   //       "==END==\n"
   // Each event resolves to (module, nid) by joining slotAddr against the
   // manifest. NID -> symbolic name comes from the existing NidNames db.
   public static class TraceCapture
   {
      public const int HeaderBytes = 16;
      public const int EventBytes  = 16;
      public const uint SupportedVersion = 3;

      public class Event
      {
         public int    Index;
         public uint   SlotAddr;
         public uint   R3, R4, R5;
         public string Module;
         public uint   Nid;
         public string Name;   // resolved via NidNames, or null
      }

      public class Summary
      {
         public uint Magic;     // raw u32, e.g. 'TRAC'
         public uint Version;
         public uint SlotsRequested, SlotsArmed, SlotsDropped;
         public uint EventsWritten, RingDropped;
         public int  TotalEvents;
         public int  ManifestSlots;
      }

      public class Result
      {
         public Summary       Summary;
         public List<Event>   Events;
         public string        Error;   // non-null on parse failure
      }

      public static Result Parse(byte[] bytes)
      {
         var result = new Result { Events = new List<Event>(), Summary = new Summary() };
         if (bytes == null || bytes.Length < HeaderBytes) {
            result.Error = "file too small for header";
            return result;
         }
         string magic = Encoding.ASCII.GetString(bytes, 0, 4);
         if (magic != "TRAC") {
            result.Error = "bad magic: " + magic;
            return result;
         }
         uint ver  = BeU32(bytes, 4);
         uint moff = BeU32(bytes, 8);
         if (ver != SupportedVersion) {
            result.Error = "unsupported version: " + ver;
            return result;
         }
         if (moff == 0 || moff > bytes.Length) {
            result.Error = "bad manifestOffset: " + moff + " (file " + bytes.Length + ")";
            return result;
         }
         result.Summary.Magic   = BeU32(bytes, 0);
         result.Summary.Version = ver;

         // manifest: slotAddr -> (module, nid). also parse the summary
         // block (==SUMMARY== ... ==END==) for armed/dropped/ring stats.
         var slotInfo = new Dictionary<uint, ManifestEntry>();
         string manifest = Encoding.ASCII.GetString(bytes, (int)moff, bytes.Length - (int)moff);
         ParseManifest(manifest, slotInfo, result.Summary);
         result.Summary.ManifestSlots = slotInfo.Count;

         int eventRegion = (int)moff - HeaderBytes;
         int count = eventRegion / EventBytes;
         result.Summary.TotalEvents = count;
         for (int i = 0; i < count; i++) {
            int off = HeaderBytes + i * EventBytes;
            uint slot = BeU32(bytes, off);
            var ev = new Event {
               Index    = i,
               SlotAddr = slot,
               R3       = BeU32(bytes, off + 4),
               R4       = BeU32(bytes, off + 8),
               R5       = BeU32(bytes, off + 12)
            };
            ManifestEntry m;
            if (slotInfo.TryGetValue(slot, out m)) {
               ev.Module = m.Module;
               ev.Nid    = m.Nid;
               ev.Name   = NidNames.Resolve(m.Nid).Name;
            } else {
               ev.Module = "?";
               ev.Nid    = 0;
               ev.Name   = null;
            }
            result.Events.Add(ev);
         }
         return result;
      }

      private struct ManifestEntry { public string Module; public uint Nid; }

      private static void ParseManifest(string text, Dictionary<uint, ManifestEntry> slots, Summary sum)
      {
         foreach (string raw in text.Split('\n')) {
            string line = raw.TrimEnd('\r');
            if (line.Length == 0) continue;
            if (line.StartsWith("slot ")) {
               // "slot 0x<addr>\t<module>\t0x<nid>"
               string[] parts = line.Split('\t');
               if (parts.Length < 3) continue;
               string addrTok = parts[0].Substring(5).Trim();   // strip "slot "
               uint addr, nid;
               if (!TryParseHex(addrTok, out addr)) continue;
               if (!TryParseHex(parts[2], out nid))  continue;
               slots[addr] = new ManifestEntry { Module = parts[1], Nid = nid };
            } else if (line.StartsWith("slots\t")) {
               // "slots\trequested=N armed=N dropped=N"
               ParseKv(line, "requested=", ref sum.SlotsRequested);
               ParseKv(line, "armed=",     ref sum.SlotsArmed);
               ParseKv(line, "dropped=",   ref sum.SlotsDropped);
            } else if (line.StartsWith("events\t")) {
               // "events\twritten=N ring_dropped=N"
               ParseKv(line, "written=",      ref sum.EventsWritten);
               ParseKv(line, "ring_dropped=", ref sum.RingDropped);
            }
         }
      }

      private static bool TryParseHex(string tok, out uint value)
      {
         value = 0;
         if (string.IsNullOrEmpty(tok)) return false;
         string s = tok.Trim();
         if (s.StartsWith("0x") || s.StartsWith("0X")) s = s.Substring(2);
         return uint.TryParse(s, System.Globalization.NumberStyles.HexNumber,
                              System.Globalization.CultureInfo.InvariantCulture, out value);
      }

      private static void ParseKv(string line, string key, ref uint dest)
      {
         int i = line.IndexOf(key, StringComparison.Ordinal);
         if (i < 0) return;
         int j = i + key.Length;
         int k = j;
         while (k < line.Length && line[k] >= '0' && line[k] <= '9') k++;
         if (k == j) return;
         uint v;
         if (uint.TryParse(line.Substring(j, k - j), out v)) dest = v;
      }

      private static uint BeU32(byte[] b, int off)
      {
         return ((uint)b[off]     << 24)
             | ((uint)b[off + 1] << 16)
             | ((uint)b[off + 2] <<  8)
             |  (uint)b[off + 3];
      }
   }
}
