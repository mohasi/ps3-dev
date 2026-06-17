using System.Collections.Generic;
using System.IO;
using System.Text;

namespace RenpyToPs3.RenPy
{
   public sealed class RpkEntry
   {
      public string Name;
      public byte[] Data;
      public RpkEntry(string name, byte[] data) { Name = name; Data = data; }
   }

   public sealed class RpkTocEntry
   {
      public string Name;
      public long Offset;
      public long Length;
   }

   // Single-file game bundle (.rpk). Uncompressed so the PS3 runtime can seek straight to
   // an asset by offset. Layout:
   //   magic "RPK1" | u32 version | u32 entryCount
   //   TOC: entryCount * { u32 nameLen; name(utf8); u64 offset; u64 length }
   //   blobs: concatenated entry data
   public static class Rpk
   {
      private static readonly byte[] Magic = Encoding.ASCII.GetBytes("RPK1");
      public const uint Version = 1;

      public static void Write(string path, IList<RpkEntry> entries)
      {
         byte[][] names = new byte[entries.Count][];
         long tocSize = 0;
         for (int i = 0; i < entries.Count; i++)
         {
            names[i] = Encoding.UTF8.GetBytes(entries[i].Name.Replace('\\', '/'));
            tocSize += 4 + names[i].Length + 8 + 8;
         }
         long blobStart = 4 + 4 + 4 + tocSize;

         using (FileStream fs = File.Create(path))
         using (BinaryWriter w = new BinaryWriter(fs))
         {
            w.Write(Magic);
            w.Write(Version);
            w.Write((uint)entries.Count);

            long off = blobStart;
            for (int i = 0; i < entries.Count; i++)
            {
               w.Write((uint)names[i].Length);
               w.Write(names[i]);
               w.Write((ulong)off);
               w.Write((ulong)entries[i].Data.Length);
               off += entries[i].Data.Length;
            }
            for (int i = 0; i < entries.Count; i++) w.Write(entries[i].Data);
         }
      }

      public static List<RpkTocEntry> ReadToc(string path)
      {
         using (FileStream fs = File.OpenRead(path))
         using (BinaryReader r = new BinaryReader(fs))
         {
            byte[] m = r.ReadBytes(4);
            if (m.Length != 4 || m[0] != Magic[0] || m[1] != Magic[1] || m[2] != Magic[2] || m[3] != Magic[3])
               throw new InvalidDataException("not an .rpk (bad magic)");
            r.ReadUInt32(); // version
            uint count = r.ReadUInt32();
            List<RpkTocEntry> toc = new List<RpkTocEntry>((int)count);
            for (uint i = 0; i < count; i++)
            {
               uint nlen = r.ReadUInt32();
               RpkTocEntry e = new RpkTocEntry();
               e.Name = Encoding.UTF8.GetString(r.ReadBytes((int)nlen));
               e.Offset = (long)r.ReadUInt64();
               e.Length = (long)r.ReadUInt64();
               toc.Add(e);
            }
            return toc;
         }
      }
   }
}
