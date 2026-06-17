using System;
using System.Collections.Generic;
using System.Text;

namespace RenpyToPs3.RenPy
{
   // Scans a raw (decompressed) pickle byte stream and collects the set of Python classes
   // it references (the Ren'Py AST node types a script uses), without reconstructing objects.
   // Handles both global encodings: GLOBAL ('c': module\n name\n) and STACK_GLOBAL (0x93:
   // module/name pushed as strings just before it). To resolve STACK_GLOBAL we track the two
   // most recently produced strings plus the string memo (PUT/BINPUT/MEMOIZE + GET/BINGET).
   public static class PickleClassScanner
   {
      public static HashSet<string> ScanClasses(byte[] b)
      {
         return new Scanner().Run(b);
      }

      private sealed class Scanner
      {
         private readonly HashSet<string> classes = new HashSet<string>(StringComparer.Ordinal);
         private readonly Dictionary<long, string> memo = new Dictionary<long, string>();
         private long memoCounter = 0;
         private string last1, last2, lastProduced;

         private void Produce(string s) { lastProduced = s; if (s != null) { last1 = last2; last2 = s; } }
         private void ProduceNull() { lastProduced = null; }
         private string MemoGet(long idx) { string v; return memo.TryGetValue(idx, out v) ? v : null; }

         public HashSet<string> Run(byte[] b)
         {
            int i = 0, n = b.Length;
            while (i < n)
            {
               byte op = b[i++];
               switch ((char)op)
               {
                  case '\x80': i += 1; break;                 // PROTO
                  case '\x95': i += 8; break;                 // FRAME

                  case '\x8c': { int len = b[i++]; Produce(Compat.Utf8(b, i, len)); i += len; break; }            // SHORT_BINUNICODE
                  case 'X': { int len = I32(b, ref i); Produce(Compat.Utf8(b, i, len)); i += len; break; }         // BINUNICODE
                  case '\x8d': { long len = I64(b, ref i); Produce(Compat.Utf8(b, i, (int)len)); i += (int)len; break; } // BINUNICODE8
                  case 'U': { int len = b[i++]; Produce(Compat.Latin1(b, i, len)); i += len; break; }              // SHORT_BINSTRING
                  case 'T': { int len = I32(b, ref i); Produce(Compat.Latin1(b, i, len)); i += len; break; }       // BINSTRING
                  case 'S': { int e = NewlineEnd(b, i); Produce(Compat.Latin1(b, i, e - i)); i = e + 1; break; }   // STRING
                  case 'V': { int e = NewlineEnd(b, i); Produce(Compat.Utf8(b, i, e - i)); i = e + 1; break; }     // UNICODE

                  case 'C': { int len = b[i++]; i += len; ProduceNull(); break; }                  // SHORT_BINBYTES
                  case 'B': { int len = I32(b, ref i); i += len; ProduceNull(); break; }            // BINBYTES
                  case '\x8e': { long len = I64(b, ref i); i += (int)len; ProduceNull(); break; }   // BINBYTES8
                  case '\x96': { long len = I64(b, ref i); i += (int)len; ProduceNull(); break; }   // BYTEARRAY8

                  case 'I': case 'L': case 'F': { i = NewlineEnd(b, i) + 1; ProduceNull(); break; } // INT/LONG/FLOAT (text)
                  case 'J': i += 4; ProduceNull(); break;     // BININT
                  case 'K': i += 1; ProduceNull(); break;     // BININT1
                  case 'M': i += 2; ProduceNull(); break;     // BININT2
                  case 'G': i += 8; ProduceNull(); break;     // BINFLOAT
                  case '\x8a': { int len = b[i++]; i += len; ProduceNull(); break; }   // LONG1
                  case '\x8b': { int len = I32(b, ref i); i += len; ProduceNull(); break; } // LONG4

                  case '\x94': memo[memoCounter++] = lastProduced; break;     // MEMOIZE
                  case 'q': { int idx = b[i++]; memo[idx] = lastProduced; break; } // BINPUT
                  case 'r': { int idx = I32(b, ref i); memo[idx] = lastProduced; break; } // LONG_BINPUT
                  case 'p': { int e = NewlineEnd(b, i); memo[Plong(b, i, e)] = lastProduced; i = e + 1; break; } // PUT
                  case 'h': { int idx = b[i++]; Produce(MemoGet(idx)); break; }      // BINGET
                  case 'j': { int idx = I32(b, ref i); Produce(MemoGet(idx)); break; } // LONG_BINGET
                  case 'g': { int e = NewlineEnd(b, i); Produce(MemoGet(Plong(b, i, e))); i = e + 1; break; } // GET

                  case 'c': // GLOBAL: module\n name\n
                     {
                        int e1 = NewlineEnd(b, i); string mod = Compat.Latin1(b, i, e1 - i); i = e1 + 1;
                        int e2 = NewlineEnd(b, i); string nm = Compat.Latin1(b, i, e2 - i); i = e2 + 1;
                        classes.Add(mod + "." + nm);
                        ProduceNull();
                        break;
                     }
                  case '\x93': // STACK_GLOBAL
                     if (last1 != null && last2 != null) classes.Add(last1 + "." + last2);
                     ProduceNull();
                     break;

                  case '\x82': i += 1; break;   // EXT1
                  case '\x83': i += 2; break;   // EXT2
                  case '\x84': i += 4; break;   // EXT4

                  default: break;
               }
            }
            return classes;
         }
      }

      private static int I32(byte[] b, ref int i) { int v = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | b[i + 3] << 24; i += 4; return v; }
      private static long I64(byte[] b, ref int i) { long v = 0; for (int k = 7; k >= 0; k--) v = (v << 8) | b[i + k]; i += 8; return v; }
      private static int NewlineEnd(byte[] b, int i) { while (i < b.Length && b[i] != (byte)'\n') i++; return i; }
      private static long Plong(byte[] b, int s, int e) { long v; return long.TryParse(Compat.Latin1(b, s, e - s), out v) ? v : -1; }
   }
}
