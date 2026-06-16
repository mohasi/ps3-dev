using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Runtime.CompilerServices;
using System.Text;

namespace RenpyToPs3.RenPy
{
    // .NET 4.0 compatibility shims (also valid on modern .NET, so the project can be
    // verified on a newer runtime before building with the in-box v4.0 toolchain).
    internal static class Compat
    {
        // Latin-1: one char per byte (round-trips bytes 0-255). Replaces Encoding.Latin1
        // (which only exists as a named property on .NET 5+).
        public static string Latin1(byte[] b, int offset, int len)
        {
            char[] c = new char[len];
            for (int k = 0; k < len; k++) c[k] = (char)b[offset + k];
            return new string(c);
        }

        public static byte[] Latin1Bytes(string s)
        {
            byte[] b = new byte[s.Length];
            for (int k = 0; k < s.Length; k++) b[k] = (byte)s[k];
            return b;
        }

        public static string Utf8(byte[] b, int offset, int len)
        {
            return Encoding.UTF8.GetString(b, offset, len);
        }

        // Inflate a zlib stream (RFC 1950): 2-byte header + raw DEFLATE + adler32.
        // .NET 4.0 has DeflateStream (raw deflate) but not ZLibStream (.NET 6+), so skip
        // the 2-byte header; trailing adler bytes are ignored by DeflateStream.
        public static byte[] Inflate(byte[] data, int offset, int count)
        {
            using (MemoryStream ms = new MemoryStream(data, offset + 2, count - 2))
            using (DeflateStream ds = new DeflateStream(ms, CompressionMode.Decompress))
            using (MemoryStream outMs = new MemoryStream())
            {
                byte[] buf = new byte[65536];
                int n;
                while ((n = ds.Read(buf, 0, buf.Length)) > 0) outMs.Write(buf, 0, n);
                return outMs.ToArray();
            }
        }
    }

    // Reference-identity comparer (ReferenceEqualityComparer is .NET 5+).
    internal sealed class RefComparer : IEqualityComparer<object>
    {
        public static readonly RefComparer Instance = new RefComparer();
        public new bool Equals(object a, object b) { return ReferenceEquals(a, b); }
        public int GetHashCode(object o) { return RuntimeHelpers.GetHashCode(o); }
    }
}
