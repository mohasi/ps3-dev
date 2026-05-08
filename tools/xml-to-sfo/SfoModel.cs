using System.Collections.Generic;

namespace XmlToSfo
{
    // PSF param data formats. Mirrors Sony's PARAM.SFO entry "data_fmt" field.
    internal enum SfoDataType : ushort
    {
        // utf8 string, NOT null-terminated. Used for the 'special' string slots
        // (legacy / variable-length notes). Rare in homebrew SFOs.
        Utf8Special = 0x0004,

        // utf8 string, null-terminated. The common case (TITLE, TITLE_ID, ...).
        Utf8 = 0x0204,

        // 32-bit little-endian integer (BOOTABLE, RESOLUTION, ATTRIBUTE, ...).
        Int4 = 0x0404
    }

    internal sealed class SfoParam
    {
        public string Key;          // <param key="...">
        public SfoDataType Type;    // mapped from <type>
        public int MaxLength;       // data slot size (max bytes)
        public byte[] Payload;      // exact bytes written into the slot
    }

    internal sealed class SfoModel
    {
        public List<SfoParam> Params = new List<SfoParam>();
    }
}
