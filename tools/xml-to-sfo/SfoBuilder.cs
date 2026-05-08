using System;
using System.IO;
using System.Text;

namespace XmlToSfo
{
    // Writes a PSF (PARAM.SFO) file from an SfoModel. Layout:
    //
    //   header (0x14 bytes)
    //   index  (0x10 bytes per entry)
    //   key table   (null-terminated keys, padded to 4-byte align)
    //   data table  (each slot = MaxLength bytes, zero-padded after Payload)
    //
    // Header fields are little-endian. Entries are sorted by key so the output
    // matches what Sony's tools produce (and what existing PARAM.SFOs in the
    // wild look like — TITLE_ID after TITLE, alphabetical).
    internal static class SfoBuilder
    {
        private const uint HeaderMagic   = 0x46535000u; // "\0PSF" in LE
        private const uint HeaderVersion = 0x00000101u; // 1.1

        public static void Write(SfoModel model, string outputPath)
        {
            // Stable alphabetical order.
            model.Params.Sort(delegate(SfoParam a, SfoParam b)
            {
                return string.CompareOrdinal(a.Key, b.Key);
            });

            int n = model.Params.Count;

            // --- key table layout ---
            // Each key is stored as ASCII + 1 null byte. Compute offsets.
            ushort[] keyOffsets = new ushort[n];
            int keyTableLen = 0;
            byte[][] keyBytes = new byte[n][];
            for (int i = 0; i < n; i++)
            {
                keyBytes[i] = Encoding.ASCII.GetBytes(model.Params[i].Key);
                keyOffsets[i] = (ushort)keyTableLen;
                keyTableLen += keyBytes[i].Length + 1;
                if (keyOffsets[i] != keyTableLen - keyBytes[i].Length - 1)
                    throw new InvalidOperationException("key table overflows ushort range");
            }
            int keyTableLenPadded = AlignUp(keyTableLen, 4);

            // --- data table layout ---
            uint[] dataOffsets = new uint[n];
            int dataCursor = 0;
            for (int i = 0; i < n; i++)
            {
                dataOffsets[i] = (uint)dataCursor;
                dataCursor += model.Params[i].MaxLength;
            }
            int dataTableLen = dataCursor;

            // --- absolute offsets ---
            int headerLen = 0x14;
            int indexLen = n * 0x10;
            uint keyTableStart = (uint)(headerLen + indexLen);
            uint dataTableStart = (uint)(keyTableStart + keyTableLenPadded);
            int totalLen = (int)dataTableStart + dataTableLen;

            byte[] buf = new byte[totalLen];

            // header
            WriteUInt32(buf, 0x00, HeaderMagic);
            WriteUInt32(buf, 0x04, HeaderVersion);
            WriteUInt32(buf, 0x08, keyTableStart);
            WriteUInt32(buf, 0x0C, dataTableStart);
            WriteUInt32(buf, 0x10, (uint)n);

            // index entries
            for (int i = 0; i < n; i++)
            {
                int eOff = headerLen + i * 0x10;
                var p = model.Params[i];
                WriteUInt16(buf, eOff + 0x00, keyOffsets[i]);
                WriteUInt16(buf, eOff + 0x02, (ushort)p.Type);
                WriteUInt32(buf, eOff + 0x04, (uint)p.Payload.Length);
                WriteUInt32(buf, eOff + 0x08, (uint)p.MaxLength);
                WriteUInt32(buf, eOff + 0x0C, dataOffsets[i]);
            }

            // key table
            int keyCursor = (int)keyTableStart;
            for (int i = 0; i < n; i++)
            {
                Buffer.BlockCopy(keyBytes[i], 0, buf, keyCursor, keyBytes[i].Length);
                keyCursor += keyBytes[i].Length;
                buf[keyCursor++] = 0; // null terminator
            }
            // remaining bytes up to dataTableStart are already 0 (zeroed by new[])

            // data table
            for (int i = 0; i < n; i++)
            {
                int absOff = (int)dataTableStart + (int)dataOffsets[i];
                Buffer.BlockCopy(model.Params[i].Payload, 0, buf, absOff, model.Params[i].Payload.Length);
                // remaining bytes already zero
            }

            File.WriteAllBytes(outputPath, buf);
        }

        private static int AlignUp(int value, int alignment)
        {
            int rem = value % alignment;
            return rem == 0 ? value : value + (alignment - rem);
        }

        private static void WriteUInt16(byte[] buf, int off, ushort v)
        {
            buf[off + 0] = (byte)(v & 0xFF);
            buf[off + 1] = (byte)((v >> 8) & 0xFF);
        }

        private static void WriteUInt32(byte[] buf, int off, uint v)
        {
            buf[off + 0] = (byte)(v & 0xFF);
            buf[off + 1] = (byte)((v >> 8) & 0xFF);
            buf[off + 2] = (byte)((v >> 16) & 0xFF);
            buf[off + 3] = (byte)((v >> 24) & 0xFF);
        }
    }
}
