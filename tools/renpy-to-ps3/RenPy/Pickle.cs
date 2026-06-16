using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace RenpyToPs3.RenPy
{
    // A reference to a Python class/callable encountered via GLOBAL / STACK_GLOBAL.
    public sealed class GlobalRef
    {
        public readonly string Module;
        public readonly string Name;
        public GlobalRef(string module, string name) { Module = module; Name = name; }
        public string Full { get { return Module + "." + Name; } }
        public override string ToString() { return Full; }
    }

    // A captured Python object instance (unknown class): class name, ctor args, BUILD state,
    // and—if it behaved as a list/dict during unpickling—the collected items.
    public sealed class PyObject
    {
        public readonly string ClassName;
        public object[] Args;
        public object State;
        public List<object> ListItems;
        public Dictionary<object, object> DictItems;

        public PyObject(string className) { ClassName = className; }

        public List<object> AsList() { if (ListItems == null) ListItems = new List<object>(); return ListItems; }
        public Dictionary<object, object> AsDict() { if (DictItems == null) DictItems = new Dictionary<object, object>(); return DictItems; }
        public override string ToString() { return "<" + ClassName + ">"; }
    }

    // Minimal, dependency-free Python pickle reader (protocols 0/2-5 subset Ren'Py emits).
    // Produces plain CLR structures: dict->Dictionary<object,object>, list->List<object>,
    // tuple->object[], str->string (Latin-1), unicode->string (UTF-8), bytes->byte[],
    // class instances->PyObject.
    public static class PickleReader
    {
        private static readonly object Mark = new object();

        public static object Load(byte[] b) { return new Machine().Run(b); }

        private sealed class Machine
        {
            private readonly List<object> stack = new List<object>(64);
            private readonly Dictionary<int, object> memo = new Dictionary<int, object>();

            private object Top() { return stack[stack.Count - 1]; }
            private void Push(object o) { stack.Add(o); }
            private object PopValue() { object v = stack[stack.Count - 1]; stack.RemoveAt(stack.Count - 1); return v; }
            private object MemoGet(int idx) { object v; return memo.TryGetValue(idx, out v) ? v : null; }

            private int PopMarkIndex()
            {
                for (int k = stack.Count - 1; k >= 0; k--)
                    if (ReferenceEquals(stack[k], Mark)) return k;
                throw new InvalidDataException("pickle: mark not found");
            }

            private object[] PopToMarkTuple()
            {
                int m = PopMarkIndex();
                int cnt = stack.Count - m - 1;
                object[] items = new object[cnt];
                for (int k = 0; k < cnt; k++) items[k] = stack[m + 1 + k];
                stack.RemoveRange(m, stack.Count - m);
                return items;
            }

            public object Run(byte[] b)
            {
                int i = 0, n = b.Length;
                while (i < n)
                {
                    byte op = b[i++];
                    switch ((char)op)
                    {
                        case '\x80': i++; break;                          // PROTO
                        case '\x95': i += 8; break;                       // FRAME
                        case '.': return Top();                           // STOP

                        case '(': Push(Mark); break;                      // MARK
                        case '}': Push(new Dictionary<object, object>()); break; // EMPTY_DICT
                        case ']': Push(new List<object>()); break;        // EMPTY_LIST
                        case ')': Push(new object[0]); break;             // EMPTY_TUPLE
                        case '\x8f': Push(new HashSet<object>()); break;  // EMPTY_SET

                        case 'N': Push(null); break;                      // NONE
                        case '\x88': Push(true); break;                   // NEWTRUE
                        case '\x89': Push(false); break;                  // NEWFALSE

                        case 'J': Push(I32(b, ref i)); break;             // BININT
                        case 'K': Push((int)b[i++]); break;               // BININT1
                        case 'M': Push(b[i] | b[i + 1] << 8); i += 2; break; // BININT2
                        case 'I': Push(ParseIntText(ReadLine(b, ref i))); break;
                        case 'L': { string s = ReadLine(b, ref i); if (s.EndsWith("L")) s = s.Substring(0, s.Length - 1); Push(ParseIntText(s)); break; }
                        case '\x8a': { int len = b[i++]; Push(LongLE(b, ref i, len)); break; } // LONG1
                        case '\x8b': { int len = I32(b, ref i); Push(LongLE(b, ref i, len)); break; } // LONG4

                        case 'G': Push(BinFloat(b, ref i)); break;        // BINFLOAT
                        case 'F': Push(double.Parse(ReadLine(b, ref i), CultureInfo.InvariantCulture)); break;

                        case 'U': { int len = b[i++]; Push(Latin1(b, ref i, len)); break; }       // SHORT_BINSTRING
                        case 'T': { int len = I32(b, ref i); Push(Latin1(b, ref i, len)); break; } // BINSTRING
                        case 'S': Push(Unquote(ReadLine(b, ref i))); break;                       // STRING

                        case '\x8c': { int len = b[i++]; Push(Utf8(b, ref i, len)); break; }        // SHORT_BINUNICODE
                        case 'X': { int len = I32(b, ref i); Push(Utf8(b, ref i, len)); break; }    // BINUNICODE
                        case '\x8d': { long len = I64(b, ref i); Push(Utf8(b, ref i, (int)len)); break; } // BINUNICODE8
                        case 'V': Push(ReadLine(b, ref i)); break;        // UNICODE

                        case 'C': { int len = b[i++]; Push(Bytes(b, ref i, len)); break; }          // SHORT_BINBYTES
                        case 'B': { int len = I32(b, ref i); Push(Bytes(b, ref i, len)); break; }    // BINBYTES
                        case '\x8e': { long len = I64(b, ref i); Push(Bytes(b, ref i, (int)len)); break; } // BINBYTES8
                        case '\x96': { long len = I64(b, ref i); Push(Bytes(b, ref i, (int)len)); break; } // BYTEARRAY8

                        case 't': Push(PopToMarkTuple()); break;          // TUPLE
                        case '\x85': { object a = stack[stack.Count - 1]; stack.RemoveAt(stack.Count - 1); Push(new object[] { a }); break; } // TUPLE1
                        case '\x86': { object y = stack[stack.Count - 1]; object x = stack[stack.Count - 2]; stack.RemoveRange(stack.Count - 2, 2); Push(new object[] { x, y }); break; } // TUPLE2
                        case '\x87': { object z = stack[stack.Count - 1]; object y = stack[stack.Count - 2]; object x = stack[stack.Count - 3]; stack.RemoveRange(stack.Count - 3, 3); Push(new object[] { x, y, z }); break; } // TUPLE3

                        case 'l': { object[] items = PopToMarkTuple(); Push(new List<object>(items)); break; } // LIST
                        case 'a': { object v = PopValue(); AppendOne(Top(), v); break; }                       // APPEND
                        case 'e': { object[] items = PopToMarkTuple(); AppendMany(Top(), items); break; }      // APPENDS

                        case 'd': { object[] items = PopToMarkTuple(); Dictionary<object, object> dd = new Dictionary<object, object>(); for (int k = 0; k + 1 < items.Length; k += 2) dd[items[k]] = items[k + 1]; Push(dd); break; } // DICT
                        case 's': { object v = PopValue(); object k = PopValue(); SetItem(Top(), k, v); break; } // SETITEM
                        case 'u': { object[] items = PopToMarkTuple(); SetItems(Top(), items); break; }          // SETITEMS

                        case '\x90': { object[] items = PopToMarkTuple(); HashSet<object> hs = Top() as HashSet<object>; if (hs != null) foreach (object it in items) hs.Add(it); break; } // ADDITEMS

                        case '\x94': memo[memo.Count] = Top(); break;     // MEMOIZE
                        case 'q': memo[b[i++]] = Top(); break;            // BINPUT
                        case 'r': { int idx = I32(b, ref i); memo[idx] = Top(); break; } // LONG_BINPUT
                        case 'p': { int e = NewlineEnd(b, i); memo[(int)ParseLong(Compat.Latin1(b, i, e - i))] = Top(); i = e + 1; break; } // PUT
                        case 'h': Push(MemoGet(b[i++])); break;           // BINGET
                        case 'j': { int idx = I32(b, ref i); Push(MemoGet(idx)); break; } // LONG_BINGET
                        case 'g': { int e = NewlineEnd(b, i); Push(MemoGet((int)ParseLong(Compat.Latin1(b, i, e - i)))); i = e + 1; break; } // GET

                        case 'c': { string mod = ReadLine(b, ref i); string nm = ReadLine(b, ref i); Push(new GlobalRef(mod, nm)); break; } // GLOBAL
                        case '\x93': { object nm = PopValue(); object mod = PopValue(); Push(new GlobalRef(mod == null ? "" : mod.ToString(), nm == null ? "" : nm.ToString())); break; } // STACK_GLOBAL
                        case 'R': { object args = PopValue(); object callable = PopValue(); Push(Construct(callable, args)); break; } // REDUCE
                        case '\x81': { object args = PopValue(); object cls = PopValue(); Push(Construct(cls, args)); break; } // NEWOBJ
                        case '\x92': { PopValue(); object args = PopValue(); object cls = PopValue(); Push(Construct(cls, args)); break; } // NEWOBJ_EX
                        case 'o': { object[] items = PopToMarkTuple(); object cls = items.Length > 0 ? items[0] : null; Push(Construct(cls, SubArray(items, 1))); break; } // OBJ
                        case 'i': { string mod = ReadLine(b, ref i); string nm = ReadLine(b, ref i); object[] items = PopToMarkTuple(); Push(Construct(new GlobalRef(mod, nm), items)); break; } // INST
                        case 'b': { object state = PopValue(); Build(Top(), state); break; } // BUILD

                        case '\x82': i++; break;   // EXT1
                        case '\x83': i += 2; break;// EXT2
                        case '\x84': i += 4; break;// EXT4

                        default:
                            throw new InvalidDataException("pickle: unhandled opcode 0x" + ((int)op).ToString("X2") + " at " + (i - 1));
                    }
                }
                throw new InvalidDataException("pickle: no STOP opcode");
            }
        }

        private static object[] SubArray(object[] a, int start)
        {
            int n = a.Length - start;
            if (n <= 0) return new object[0];
            object[] r = new object[n];
            Array.Copy(a, start, r, 0, n);
            return r;
        }

        private static void AppendOne(object target, object value)
        {
            List<object> l = target as List<object>;
            if (l != null) { l.Add(value); return; }
            PyObject p = target as PyObject;
            if (p != null) p.AsList().Add(value);
        }
        private static void AppendMany(object target, object[] items)
        {
            List<object> l = target as List<object>;
            if (l != null) { l.AddRange(items); return; }
            PyObject p = target as PyObject;
            if (p != null) p.AsList().AddRange(items);
        }
        private static void SetItem(object target, object key, object value)
        {
            Dictionary<object, object> d = target as Dictionary<object, object>;
            if (d != null) { d[key] = value; return; }
            PyObject p = target as PyObject;
            if (p != null) p.AsDict()[key] = value;
        }
        private static void SetItems(object target, object[] items)
        {
            for (int k = 0; k + 1 < items.Length; k += 2) SetItem(target, items[k], items[k + 1]);
        }

        private static object Construct(object callable, object argsObj)
        {
            object[] args = argsObj as object[];
            if (args == null) args = (argsObj == null) ? new object[0] : new object[] { argsObj };

            GlobalRef g = callable as GlobalRef;
            if (g != null)
            {
                string full = g.Full;
                if (full == "copy_reg._reconstructor" || full == "copyreg._reconstructor")
                {
                    GlobalRef cg = args.Length > 0 ? args[0] as GlobalRef : null;
                    PyObject obj = new PyObject(cg != null ? cg.Full : "object");
                    GlobalRef bg = args.Length > 1 ? args[1] as GlobalRef : null;
                    if (bg != null)
                    {
                        if (bg.Name == "list") obj.AsList();
                        else if (bg.Name == "dict" || bg.Name == "OrderedDict" || bg.Name == "defaultdict") obj.AsDict();
                    }
                    return obj;
                }
                if (full == "copy_reg.__newobj__" || full == "copyreg.__newobj__")
                    return Construct(args.Length > 0 ? args[0] : null, args.Length > 1 ? SubArray(args, 1) : new object[0]);
                if (full == "builtins.list" || full == "__builtin__.list")
                {
                    List<object> l = new List<object>();
                    if (args.Length > 0 && !(args[0] is string))
                    {
                        IEnumerable e = args[0] as IEnumerable;
                        if (e != null) foreach (object it in e) l.Add(it);
                    }
                    return l;
                }
                if (full == "builtins.dict" || full == "__builtin__.dict" || full == "collections.OrderedDict" || full == "collections.defaultdict")
                    return new Dictionary<object, object>();
                if (full == "builtins.set" || full == "__builtin__.set")
                    return new HashSet<object>();

                PyObject po = new PyObject(g.Full);
                po.Args = args;
                return po;
            }

            PyObject p2 = new PyObject(callable == null ? "?" : callable.ToString());
            p2.Args = args;
            return p2;
        }

        private static void Build(object target, object state)
        {
            PyObject p = target as PyObject;
            if (p != null) { p.State = state; return; }
            Dictionary<object, object> d = target as Dictionary<object, object>;
            Dictionary<object, object> sd = state as Dictionary<object, object>;
            if (d != null && sd != null)
                foreach (KeyValuePair<object, object> kv in sd) d[kv.Key] = kv.Value;
        }

        // ---- byte readers ----
        private static int I32(byte[] b, ref int i) { int v = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | b[i + 3] << 24; i += 4; return v; }
        private static long I64(byte[] b, ref int i) { long v = 0; for (int k = 7; k >= 0; k--) v = v << 8 | b[i + k]; i += 8; return v; }

        private static object LongLE(byte[] b, ref int i, int len)
        {
            if (len == 0) return 0;
            if (len > 8) throw new NotSupportedException("pickle LONG larger than 64-bit not supported");
            long v = 0;
            for (int k = len - 1; k >= 0; k--) v = (v << 8) | b[i + k];
            if (len < 8 && (b[i + len - 1] & 0x80) != 0)               // sign-extend negatives
                for (int k = len; k < 8; k++) v |= ((long)0xFF) << (k * 8);
            i += len;
            if (v >= int.MinValue && v <= int.MaxValue) return (int)v;
            return v;
        }

        private static double BinFloat(byte[] b, ref int i)
        {
            byte[] tmp = new byte[8];
            for (int k = 0; k < 8; k++) tmp[k] = b[i + 7 - k];   // big-endian -> little-endian
            i += 8;
            return BitConverter.ToDouble(tmp, 0);
        }

        private static string Latin1(byte[] b, ref int i, int len) { string s = Compat.Latin1(b, i, len); i += len; return s; }
        private static string Utf8(byte[] b, ref int i, int len) { string s = Compat.Utf8(b, i, len); i += len; return s; }
        private static byte[] Bytes(byte[] b, ref int i, int len) { byte[] r = new byte[len]; Array.Copy(b, i, r, 0, len); i += len; return r; }

        private static int NewlineEnd(byte[] b, int i) { while (i < b.Length && b[i] != (byte)'\n') i++; return i; }

        private static string ReadLine(byte[] b, ref int i)
        {
            int start = i;
            while (i < b.Length && b[i] != (byte)'\n') i++;
            string s = Compat.Latin1(b, start, i - start);
            i++;
            return s.TrimEnd('\r');
        }

        private static object ParseIntText(string s)
        {
            long l;
            if (long.TryParse(s, out l)) return (l >= int.MinValue && l <= int.MaxValue) ? (object)(int)l : (object)l;
            throw new InvalidDataException("pickle: integer too large for 64-bit");
        }

        private static long ParseLong(string s) { return long.Parse(s); }

        private static string Unquote(string s)
        {
            if (s.Length >= 2 && (s[0] == '\'' || s[0] == '"')) s = s.Substring(1, s.Length - 2);
            return s.Replace("\\n", "\n").Replace("\\t", "\t").Replace("\\'", "'").Replace("\\\\", "\\");
        }
    }
}
