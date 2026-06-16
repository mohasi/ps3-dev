using System;
using System.Collections;
using System.IO;
using System.Text;

namespace RenpyToPs3.RenPy
{
    // Loads a compiled Ren'Py script (.rpyc).
    // Containers:
    //   - Legacy (Ren'Py 6.x): whole file is a single zlib stream of a pickle.
    //   - RPYC2 (Ren'Py 7/8): magic "RENPY RPC2" + (slot,start,length) u32 triples
    //     terminated by slot 0; slot 1 holds the zlib'd pickle.
    public static class RpycFile
    {
        public static byte[] LoadPickle(string path)
        {
            return Decompress(File.ReadAllBytes(path));
        }

        public static object LoadAst(string path)
        {
            return PickleReader.Load(LoadPickle(path));
        }

        // Loads the top-level statement list of a script (the unit the compiler lowers).
        // The unpickled root is (version, key, statements) on old Ren'Py, or the list
        // itself; we take the first list whose items are renpy.ast.* nodes. Null if the
        // file holds no statements.
        public static IList LoadStatements(string path)
        {
            return FindStatementList(LoadAst(path));
        }

        public static IList FindStatementList(object root)
        {
            object[] tup = root as object[];
            if (tup != null)
                foreach (object part in tup)
                {
                    IList l = part as IList;
                    if (l != null && LooksLikeStatements(l)) return l;
                }
            IList top = root as IList;
            if (top != null && LooksLikeStatements(top)) return top;
            return null;
        }

        private static bool LooksLikeStatements(IList l)
        {
            foreach (object item in l)
            {
                PyObject p = item as PyObject;
                if (p != null && p.ClassName.StartsWith("renpy.ast.", StringComparison.Ordinal)) return true;
            }
            return false;
        }

        private static byte[] Decompress(byte[] raw)
        {
            if (raw.Length >= 10 && Encoding.ASCII.GetString(raw, 0, 10) == "RENPY RPC2")
            {
                int pos = 10;
                while (pos + 12 <= raw.Length)
                {
                    int slot = BitConverter.ToInt32(raw, pos);
                    int start = BitConverter.ToInt32(raw, pos + 4);
                    int length = BitConverter.ToInt32(raw, pos + 8);
                    pos += 12;
                    if (slot == 0) break;
                    if (slot == 1) return Compat.Inflate(raw, start, length);
                }
                throw new InvalidDataException("RPYC2 container has no data slot (1).");
            }
            return Compat.Inflate(raw, 0, raw.Length);
        }
    }
}
