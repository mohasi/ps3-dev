using System.Collections.Generic;

namespace XmlToSfo
{
   // psf param data formats. mirrors sony's param.sfo entry "data_fmt" field.
   internal enum SfoDataType : ushort
   {
      // utf8 string, NOT null-terminated. used for the 'special' string slots
      // (legacy / variable-length notes). rare in homebrew sfos.
      Utf8Special = 0x0004,

      // utf8 string, null-terminated. the common case (TITLE, TITLE_ID, ...).
      Utf8 = 0x0204,

      // 32-bit little-endian integer (BOOTABLE, RESOLUTION, ATTRIBUTE, ...).
      Int4 = 0x0404
   }

   // one psf index entry: its key, format, slot size, and the exact bytes written.
   internal sealed class SfoParam
   {
      public string Key { get; private set; }
      public SfoDataType Type { get; private set; }
      public int MaxLength { get; private set; }     // data slot size, in bytes
      public byte[] Payload { get; private set; }     // exact bytes written into the slot

      public SfoParam(string key, SfoDataType type, int maxLength, byte[] payload)
      {
         Key = key;
         Type = type;
         MaxLength = maxLength;
         Payload = payload;
      }
   }

   internal sealed class SfoModel
   {
      public List<SfoParam> Params { get; private set; }

      public SfoModel()
      {
         Params = new List<SfoParam>();
      }
   }
}
