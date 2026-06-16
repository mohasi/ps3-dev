using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace RenpyToPs3.RenPy
{
    // Reader for Ren'Py RPA archives (RPA-3.0 and RPA-2.0).
    //   Header line (ASCII, ends '\n'): "RPA-3.0 <16-hex offset> <8-hex key>" or "RPA-2.0 <offset>".
    //   Index: zlib-compressed pickle at the offset; dict { name : [ (offset,length,prefix), ... ] }.
    //   RPA-3.0 XORs each segment's offset and length with the key; prefix is stored inline and
    //   prepended to the file data (remaining length-prefix.Length bytes read from offset).
    public sealed class RpaArchive
    {
        private readonly string _path;

        public string Version { get; private set; }
        public long IndexOffset { get; private set; }
        public long Key { get; private set; }

        public sealed class Segment
        {
            public long Offset;
            public long Length;
            public byte[] Prefix = new byte[0];
            public long BodyLength { get { return Length - Prefix.Length; } }
        }

        public RpaArchive(string path)
        {
            _path = path;
            Version = "";
            ReadHeader();
        }

        private void ReadHeader()
        {
            using (FileStream fs = File.OpenRead(_path))
            {
                List<byte> line = new List<byte>(64);
                int b;
                while ((b = fs.ReadByte()) != -1 && b != '\n') line.Add((byte)b);

                string header = Encoding.ASCII.GetString(line.ToArray()).Trim();
                string[] parts = header.Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);

                if (parts.Length < 2 || !parts[0].StartsWith("RPA-"))
                    throw new InvalidDataException(
                        "Not a recognized RPA archive (RPA-1.0 archives use a separate .rpi index and are not supported).");

                Version = parts[0];
                IndexOffset = Convert.ToInt64(parts[1], 16);
                Key = parts.Length > 2 ? Convert.ToInt64(parts[2], 16) : 0;
            }
        }

        public Dictionary<string, List<Segment>> ReadIndex()
        {
            byte[] pickled;
            using (FileStream fs = File.OpenRead(_path))
            {
                fs.Seek(IndexOffset, SeekOrigin.Begin);
                long remain = fs.Length - IndexOffset;
                byte[] comp = new byte[remain];
                int read = 0;
                while (read < remain)
                {
                    int n = fs.Read(comp, read, (int)(remain - read));
                    if (n <= 0) break;
                    read += n;
                }
                pickled = Compat.Inflate(comp, 0, comp.Length);
            }

            object root = PickleReader.Load(pickled);
            IDictionary table = root as IDictionary;
            if (table == null) throw new InvalidDataException("RPA index is not a dictionary as expected.");

            Dictionary<string, List<Segment>> index =
                new Dictionary<string, List<Segment>>(table.Count, StringComparer.Ordinal);

            foreach (DictionaryEntry entry in table)
            {
                string name = CoerceName(entry.Key);
                List<Segment> segments = new List<Segment>();

                IEnumerable segs = entry.Value as IEnumerable;
                if (segs != null)
                    foreach (object segObj in segs)
                    {
                        object[] tuple = segObj as object[];
                        if (tuple == null || tuple.Length < 2) continue;

                        long offset = Convert.ToInt64(tuple[0]) ^ Key;
                        long length = Convert.ToInt64(tuple[1]) ^ Key;
                        byte[] prefix = tuple.Length > 2 ? CoercePrefix(tuple[2]) : new byte[0];

                        Segment s = new Segment();
                        s.Offset = offset; s.Length = length; s.Prefix = prefix;
                        segments.Add(s);
                    }

                index[name] = segments;
            }
            return index;
        }

        public long FileSize(List<Segment> segments)
        {
            long total = 0;
            foreach (Segment s in segments) total += s.Length;
            return total;
        }

        public byte[] ReadFile(List<Segment> segments)
        {
            using (FileStream fs = File.OpenRead(_path))
            using (MemoryStream outMs = new MemoryStream())
            {
                foreach (Segment seg in segments)
                {
                    if (seg.Prefix.Length > 0) outMs.Write(seg.Prefix, 0, seg.Prefix.Length);

                    long remaining = seg.BodyLength;
                    if (remaining < 0)
                        throw new InvalidDataException("Segment length is smaller than its prefix; archive may be corrupt.");

                    fs.Seek(seg.Offset, SeekOrigin.Begin);
                    byte[] buffer = new byte[81920];
                    while (remaining > 0)
                    {
                        int want = (int)Math.Min(buffer.Length, remaining);
                        int got = fs.Read(buffer, 0, want);
                        if (got <= 0) throw new EndOfStreamException("Unexpected end of archive while reading a file.");
                        outMs.Write(buffer, 0, got);
                        remaining -= got;
                    }
                }
                return outMs.ToArray();
            }
        }

        public void ExtractAll(string outputDir, Dictionary<string, List<Segment>> index, Action<int, int, string> progress)
        {
            int total = index.Count;
            int count = 0;
            string root = Path.GetFullPath(outputDir) + Path.DirectorySeparatorChar;

            foreach (KeyValuePair<string, List<Segment>> kvp in index)
            {
                string relative = kvp.Key.Replace('/', Path.DirectorySeparatorChar);
                string outPath = Path.GetFullPath(Path.Combine(outputDir, relative));

                if (!outPath.StartsWith(root, StringComparison.Ordinal))
                    throw new InvalidDataException("Archive entry escapes output directory: " + kvp.Key);

                string dir = Path.GetDirectoryName(outPath);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

                File.WriteAllBytes(outPath, ReadFile(kvp.Value));

                count++;
                if (progress != null) progress(count, total, kvp.Key);
            }
        }

        private static string CoerceName(object key)
        {
            if (key is string) return (string)key;
            byte[] b = key as byte[];
            if (b != null) return Encoding.UTF8.GetString(b);
            return key == null ? "" : key.ToString();
        }

        private static byte[] CoercePrefix(object prefix)
        {
            if (prefix == null) return new byte[0];
            byte[] b = prefix as byte[];
            if (b != null) return b;
            string s = prefix as string;
            if (s != null) return Compat.Latin1Bytes(s);
            return new byte[0];
        }
    }
}
