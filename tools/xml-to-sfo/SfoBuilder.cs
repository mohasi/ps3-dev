using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace XmlToSfo
{
   // writes a psf (param.sfo) binary from an sfomodel. on-disk layout, in order:
   //
   //   header     0x14 bytes  - magic, version, table offsets, entry count
   //   index      0x10 bytes per entry
   //   key table  - null-terminated keys, padded to 4-byte alignment
   //   data table - one slot per entry; the slot is maxlength bytes, zero-padded
   //
   // every integer is little-endian, which is binarywriter's native byte order.
   // entries are sorted by key so the output matches sony's tools (TITLE_ID after
   // TITLE, alphabetical) and the param.sfos already in the wild.
   internal static class SfoBuilder
   {
     private const uint Magic = 0x46535000u;     // "\0PSF"
     private const uint Version = 0x00000101u;   // 1.1
     private const int HeaderSize = 0x14;
     private const int IndexEntrySize = 0x10;

     // an entry together with where its key and data slot land in the file.
     private sealed class PlacedEntry
     {
       public SfoParam Param;
       public byte[] KeyBytes;     // ascii key, without the null terminator
       public ushort KeyOffset;    // offset within the key table
       public uint DataOffset;     // offset within the data table
     }

     private sealed class Layout
     {
       public List<PlacedEntry> Entries;
       public uint KeyTableStart;
       public uint DataTableStart;
     }

     public static void Write(SfoModel model, string outputPath)
     {
       List<SfoParam> sorted = model.Params.OrderBy(param => param.Key, StringComparer.Ordinal).ToList();
       Layout layout = PlanLayout(sorted);

       using (var stream = new MemoryStream())
       using (var writer = new BinaryWriter(stream))
       {
         WriteHeader(writer, sorted.Count, layout);
         WriteIndex(writer, layout);
         WriteKeyTable(writer, layout);
         WriteDataTable(writer, layout);
         File.WriteAllBytes(outputPath, stream.ToArray());
       }
     }

     // works out every offset up front, so the header and index can point at the
     // key and data tables before those tables are actually written.
     private static Layout PlanLayout(List<SfoParam> sorted)
     {
       var entries = new List<PlacedEntry>(sorted.Count);

       // key table: each key is ascii plus one null byte, laid end to end.
       int keyCursor = 0;
       foreach (SfoParam param in sorted)
       {
         byte[] keyBytes = Encoding.ASCII.GetBytes(param.Key);
         entries.Add(new PlacedEntry { Param = param, KeyBytes = keyBytes, KeyOffset = checked((ushort)keyCursor) });
         keyCursor += keyBytes.Length + 1;
       }

       // data table: one maxlength-sized slot per entry, laid end to end.
       uint dataCursor = 0;
       foreach (PlacedEntry entry in entries)
       {
         entry.DataOffset = dataCursor;
         dataCursor += (uint)entry.Param.MaxLength;
       }

       uint keyTableStart = (uint)(HeaderSize + sorted.Count * IndexEntrySize);
       uint dataTableStart = keyTableStart + (uint)AlignUp(keyCursor, 4);
       return new Layout { Entries = entries, KeyTableStart = keyTableStart, DataTableStart = dataTableStart };
     }

     private static void WriteHeader(BinaryWriter writer, int entryCount, Layout layout)
     {
       writer.Write(Magic);
       writer.Write(Version);
       writer.Write(layout.KeyTableStart);
       writer.Write(layout.DataTableStart);
       writer.Write((uint)entryCount);
     }

     private static void WriteIndex(BinaryWriter writer, Layout layout)
     {
       foreach (PlacedEntry entry in layout.Entries)
       {
         writer.Write(entry.KeyOffset);                   // key offset within key table
         writer.Write((ushort)entry.Param.Type);          // data format
         writer.Write((uint)entry.Param.Payload.Length);  // used bytes
         writer.Write((uint)entry.Param.MaxLength);        // slot size
         writer.Write(entry.DataOffset);                  // data offset within data table
       }
     }

     private static void WriteKeyTable(BinaryWriter writer, Layout layout)
     {
       foreach (PlacedEntry entry in layout.Entries)
       {
         writer.Write(entry.KeyBytes);
         writer.Write((byte)0);   // null terminator
       }
       PadTo(writer, layout.DataTableStart);   // align up to the data table
     }

     private static void WriteDataTable(BinaryWriter writer, Layout layout)
     {
       foreach (PlacedEntry entry in layout.Entries)
       {
         writer.Write(entry.Param.Payload);
         PadBytes(writer, entry.Param.MaxLength - entry.Param.Payload.Length);   // zero-fill the rest of the slot
       }
     }

     // ---- low-level helpers ----

     private static void PadTo(BinaryWriter writer, long targetPosition)
     {
       PadBytes(writer, (int)(targetPosition - writer.BaseStream.Position));
     }

     private static void PadBytes(BinaryWriter writer, int count)
     {
       if (count > 0)
         writer.Write(new byte[count]);
     }

     private static int AlignUp(int value, int alignment)
     {
       int remainder = value % alignment;
       return remainder == 0 ? value : value + (alignment - remainder);
     }
   }
}
