using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace RenpyToPs3.RenPy
{
   // Binary serialization of an IrProgram to the on-device bytecode format (game.rbc).
   // All integers little-endian, strings length-prefixed UTF-8:
   //   magic "RPYB" | u32 instrCount | u32 stringCount | u32 labelCount | u32 entryAddr
   //   instrs  instrCount * { u8 op; i32 a; i32 b; i32 c }   (fixed 13 bytes)
   //   strings stringCount * { u32 len; bytes utf8 }
   //   labels  labelCount  * { u32 nameLen; bytes utf8; u32 addr }
   //   exprs: u32 exprCount; each { u32 opCount; opCount * { u8 op; i32 arg } }
   //   atls:  u32 atlCount; each { i32 repeatCount; u32 keyCount;
   //                                     keyCount * { u8 warper; i32 durMs; u8 propCount;
   //                                                  propCount * { u8 prop; i32 milliValue } } }
   //   imaps: u32 count; each { str ground; str hover; u32 hotspotCount;
   //                            hotspotCount * { i32 x0; i32 y0; i32 x1; i32 y1; str value } }
   // Fixed-width instructions keep the C interpreter trivial (no per-op parsing). Single current
   // format (no versioning -- every rpk is repacked fresh): the expr + atl sections are always
   // present (count 0 if empty). The Show/Scene/Image C field is an atl-program index (-1 = none).
   public static class Bytecode
   {
      private static readonly byte[] Magic = Encoding.ASCII.GetBytes("RPYB");

      public static byte[] Write(IrProgram p)
      {
         using (MemoryStream ms = new MemoryStream())
         using (BinaryWriter w = new BinaryWriter(ms, Encoding.UTF8))
         {
            w.Write(Magic);
            w.Write((uint)p.Code.Count);
            w.Write((uint)p.Strings.Count);
            w.Write((uint)p.Labels.Count);
            int start;
            w.Write((uint)(p.Labels.TryGetValue("start", out start) ? start : 0));

            foreach (Instr ins in p.Code)
            {
               w.Write((byte)ins.Op);
               w.Write(ins.A);
               w.Write(ins.B);
               w.Write(ins.C);
            }
            foreach (string s in p.Strings) WriteStr(w, s);
            foreach (KeyValuePair<string, int> kv in p.Labels) { WriteStr(w, kv.Key); w.Write((uint)kv.Value); }

            // expr section (v2+)
            w.Write((uint)p.Exprs.Count);
            foreach (ExprProgram ep in p.Exprs)
            {
               w.Write((uint)ep.Ops.Count);
               foreach (ExprInstr e in ep.Ops) { w.Write((byte)e.Op); w.Write(e.Arg); }
            }

            // atl section
            w.Write((uint)p.Atls.Count);
            foreach (AtlProgram ap in p.Atls)
            {
               w.Write(ap.RepeatCount);
               w.Write((uint)ap.Keys.Count);
               foreach (AtlKey k in ap.Keys)
               {
                  w.Write((byte)k.Warper);
                  w.Write(k.DurMs);
                  w.Write((byte)k.Props.Count);
                  foreach (KeyValuePair<AtlProp, int> pr in k.Props) { w.Write((byte)pr.Key); w.Write(pr.Value); }
               }
            }

            // imagemap section: u32 count; each {
            //   str kind; str ground; str idle; str hover; str selectedIdle; str selectedHover;
            //   u32 hotspotCount; hotspotCount * { i32 x0; i32 y0; i32 x1; i32 y1; str name } }
            // kind == "" is the simple renpy.imagemap (only ground+hover used); otherwise a themed
            // screen ("navigation"/"load_save"/"preferences"/"yesno_prompt"/"main_menu") replayed by
            // the player's generic imagemap renderer.
            w.Write((uint)p.ImageMaps.Count);
            foreach (ImageMapDef d in p.ImageMaps)
            {
               WriteStr(w, d.Kind);
               WriteStr(w, d.Ground);
               WriteStr(w, d.Idle);
               WriteStr(w, d.Hover);
               WriteStr(w, d.SelectedIdle);
               WriteStr(w, d.SelectedHover);
               w.Write((uint)d.Hotspots.Count);
               foreach (ImageMapHotspot h in d.Hotspots)
               {
                  w.Write(h.X0); w.Write(h.Y0); w.Write(h.X1); w.Write(h.Y1);
                  WriteStr(w, h.Name);
               }
            }

            // overlay (HUD) section: u32 count; each { str name; u32 widgetCount;
            //   widgetCount * { u8 kind; i32 x; i32 y; str a; str b; str action; i32 guardExpr } }
            w.Write((uint)p.Overlays.Count);
            foreach (OverlayDef ov in p.Overlays.Values)
            {
               WriteStr(w, ov.Name);
               w.Write((uint)ov.Widgets.Count);
               foreach (OvWidget wd in ov.Widgets)
               {
                  w.Write((byte)wd.Kind);
                  w.Write(wd.X); w.Write(wd.Y);
                  WriteStr(w, wd.A); WriteStr(w, wd.B); WriteStr(w, wd.Action);
                  w.Write(wd.GuardExpr);
               }
            }

            w.Flush();
            return ms.ToArray();
         }
      }

      private static void WriteStr(BinaryWriter w, string s)
      {
         byte[] b = Encoding.UTF8.GetBytes(s);
         w.Write((uint)b.Length);
         w.Write(b);
      }

      // Decoded form used to verify round-trips (the C runtime reads the same layout).
      public sealed class Decoded
      {
         public uint EntryAddr;
         public readonly List<Instr> Code = new List<Instr>();
         public readonly List<string> Strings = new List<string>();
         public readonly Dictionary<string, int> Labels = new Dictionary<string, int>(StringComparer.Ordinal);
         public readonly List<ExprProgram> Exprs = new List<ExprProgram>();
         public readonly List<AtlProgram> Atls = new List<AtlProgram>();
      }

      public static Decoded Read(byte[] data)
      {
         using (MemoryStream ms = new MemoryStream(data))
         using (BinaryReader r = new BinaryReader(ms, Encoding.UTF8))
         {
            byte[] m = r.ReadBytes(4);
            if (m.Length != 4 || m[0] != Magic[0] || m[1] != Magic[1] || m[2] != Magic[2] || m[3] != Magic[3])
               throw new InvalidDataException("bad magic (not an .rbc)");

            Decoded d = new Decoded();
            uint instrCount = r.ReadUInt32();
            uint stringCount = r.ReadUInt32();
            uint labelCount = r.ReadUInt32();
            d.EntryAddr = r.ReadUInt32();

            for (uint i = 0; i < instrCount; i++)
               d.Code.Add(new Instr((IrOp)r.ReadByte(), r.ReadInt32(), r.ReadInt32(), r.ReadInt32()));
            for (uint i = 0; i < stringCount; i++)
               d.Strings.Add(ReadStr(r));
            for (uint i = 0; i < labelCount; i++)
            {
               string name = ReadStr(r);
               d.Labels[name] = (int)r.ReadUInt32();
            }
            if (ms.Position < ms.Length)
            {
               uint exprCount = r.ReadUInt32();
               for (uint i = 0; i < exprCount; i++)
               {
                  ExprProgram ep = new ExprProgram();
                  uint opCount = r.ReadUInt32();
                  for (uint j = 0; j < opCount; j++) ep.Emit((ExprOp)r.ReadByte(), r.ReadInt32());
                  d.Exprs.Add(ep);
               }
            }
            if (ms.Position < ms.Length)
            {
               uint atlCount = r.ReadUInt32();
               for (uint i = 0; i < atlCount; i++)
               {
                  AtlProgram ap = new AtlProgram();
                  ap.RepeatCount = r.ReadInt32();
                  uint keyCount = r.ReadUInt32();
                  for (uint k = 0; k < keyCount; k++)
                  {
                     AtlKey key = new AtlKey();
                     key.Warper = (AtlWarper)r.ReadByte();
                     key.DurMs = r.ReadInt32();
                     int pc = r.ReadByte();
                     for (int j = 0; j < pc; j++) key.Props.Add(new KeyValuePair<AtlProp, int>((AtlProp)r.ReadByte(), r.ReadInt32()));
                     ap.Keys.Add(key);
                  }
                  d.Atls.Add(ap);
               }
            }
            return d;
         }
      }

      private static string ReadStr(BinaryReader r)
      {
         uint len = r.ReadUInt32();
         return Encoding.UTF8.GetString(r.ReadBytes((int)len));
      }
   }
}
