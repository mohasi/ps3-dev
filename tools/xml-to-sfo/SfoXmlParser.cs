using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml;
using System.Xml.Linq;

namespace XmlToSfo
{
   internal sealed class ParseException : Exception
   {
      public ParseException(string message) : base(message) { }
   }

   internal static class SfoXmlParser
   {
      // psf keys are uppercase letters, digits, and underscores (e.g. PS3_SYSTEM_VER).
      // keep this strict: the parser would accept anything, but the ps3 would not.
      private static readonly Regex KeyPattern = new Regex(@"^[A-Z][A-Z0-9_]*$", RegexOptions.Compiled);

      private static readonly Dictionary<string, SfoDataType> TypeNames =
         new Dictionary<string, SfoDataType>(StringComparer.Ordinal)
      {
         { "utf8",         SfoDataType.Utf8 },
         { "utf8-special", SfoDataType.Utf8Special },
         { "int4",         SfoDataType.Int4 }
      };

      // default type and slot size for each well-known key, so the xml doesn't
      // have to repeat them. values from psdevwiki PARAM.SFO and sony sdk samples.
      private struct KeyDefaults
      {
         public readonly SfoDataType Type;
         public readonly int MaxLength;
         public KeyDefaults(SfoDataType type, int maxLength)
         {
            Type = type;
            MaxLength = maxLength;
         }
      }

      private static readonly Dictionary<string, KeyDefaults> WellKnownKeys =
         new Dictionary<string, KeyDefaults>(StringComparer.Ordinal)
      {
         { "ACCOUNT_ID",          new KeyDefaults(SfoDataType.Utf8Special, 16)   },
         { "ACCOUNTID",           new KeyDefaults(SfoDataType.Utf8,        16)   },
         { "ANALOG_MODE",         new KeyDefaults(SfoDataType.Int4,        4)    },
         { "APP_VER",             new KeyDefaults(SfoDataType.Utf8,        8)    },
         { "ATTRIBUTE",           new KeyDefaults(SfoDataType.Int4,        4)    },
         { "BOOTABLE",            new KeyDefaults(SfoDataType.Int4,        4)    },
         { "CATEGORY",            new KeyDefaults(SfoDataType.Utf8,        4)    },
         { "CONTENT_ID",          new KeyDefaults(SfoDataType.Utf8,        48)   },
         { "DETAIL",              new KeyDefaults(SfoDataType.Utf8,        1024) },
         { "GAMEDATA_ID",         new KeyDefaults(SfoDataType.Utf8,        32)   },
         { "ITEM_PRIORITY",       new KeyDefaults(SfoDataType.Int4,        4)    },
         { "LANG",                new KeyDefaults(SfoDataType.Int4,        4)    },
         { "LICENSE",             new KeyDefaults(SfoDataType.Utf8,        512)  },
         { "NP_COMMUNICATION_ID", new KeyDefaults(SfoDataType.Utf8,        16)   },
         { "NPCOMMID",            new KeyDefaults(SfoDataType.Utf8,        16)   },
         { "PADDING",             new KeyDefaults(SfoDataType.Utf8Special, 8)    },
         { "PARAMS",              new KeyDefaults(SfoDataType.Utf8Special, 1024) },
         { "PARAMS2",             new KeyDefaults(SfoDataType.Utf8Special, 12)   },
         { "PARENTAL_LEVEL",      new KeyDefaults(SfoDataType.Int4,        4)    },
         { "PARENTAL_LEVEL_A",    new KeyDefaults(SfoDataType.Int4,        4)    },
         { "PARENTAL_LEVEL_C",    new KeyDefaults(SfoDataType.Int4,        4)    },
         { "PARENTAL_LEVEL_E",    new KeyDefaults(SfoDataType.Int4,        4)    },
         { "PARENTAL_LEVEL_H",    new KeyDefaults(SfoDataType.Int4,        4)    },
         { "PARENTAL_LEVEL_J",    new KeyDefaults(SfoDataType.Int4,        4)    },
         { "PARENTAL_LEVEL_K",    new KeyDefaults(SfoDataType.Int4,        4)    },
         { "PARENTALLEVEL",       new KeyDefaults(SfoDataType.Int4,        4)    },
         { "PATCH_FILE",          new KeyDefaults(SfoDataType.Utf8,        32)   },
         { "PS3_SYSTEM_VER",      new KeyDefaults(SfoDataType.Utf8,        8)    },
         { "REGION_DENY",         new KeyDefaults(SfoDataType.Int4,        4)    },
         { "RESOLUTION",          new KeyDefaults(SfoDataType.Int4,        4)    },
         { "SAVEDATA_DETAIL",     new KeyDefaults(SfoDataType.Utf8,        1024) },
         { "SAVEDATA_DIRECTORY",  new KeyDefaults(SfoDataType.Utf8,        64)   },
         { "SAVEDATA_FILE_LIST",  new KeyDefaults(SfoDataType.Utf8Special, 3168) },
         { "SAVEDATA_LIST_PARAM", new KeyDefaults(SfoDataType.Utf8,        8)    },
         { "SAVEDATA_PARAMS",     new KeyDefaults(SfoDataType.Utf8Special, 128)  },
         { "SAVEDATA_TITLE",      new KeyDefaults(SfoDataType.Utf8,        128)  },
         { "SOUND_FORMAT",        new KeyDefaults(SfoDataType.Int4,        4)    },
         { "SOURCE",              new KeyDefaults(SfoDataType.Int4,        4)    },
         { "SUB_TITLE",           new KeyDefaults(SfoDataType.Utf8,        128)  },
         { "TARGET_APP_VER",      new KeyDefaults(SfoDataType.Utf8,        8)    },
         { "TITLE",               new KeyDefaults(SfoDataType.Utf8,        128)  },
         { "TITLE_ID",            new KeyDefaults(SfoDataType.Utf8,        16)   },
         { "VERSION",             new KeyDefaults(SfoDataType.Utf8,        8)    },
         { "XMB_APPS",            new KeyDefaults(SfoDataType.Int4,        4)    },
      };

      public static SfoModel Parse(string path)
      {
         XDocument document = LoadDocument(path);

         XElement root = document.Root;
         if (root == null || root.Name.LocalName != "paramsfo")
            throw new ParseException("Root element must be <paramsfo>.");

         var model = new SfoModel();
         var seenKeys = new HashSet<string>(StringComparer.Ordinal);
         foreach (XElement element in root.Elements())
         {
            if (element.Name.LocalName != "param")
               throw new ParseException(LocationOf(element) + "unexpected element <" + element.Name.LocalName +
                  ">; only <param> is allowed inside <paramsfo>.");

            SfoParam param = ParseParam(element);
            if (!seenKeys.Add(param.Key))
               throw new ParseException(LocationOf(element) + "duplicate key '" + param.Key + "'.");

            model.Params.Add(param);
         }

         if (model.Params.Count == 0)
            throw new ParseException("<paramsfo> contains no <param> entries.");

         return model;
      }

      private static XDocument LoadDocument(string path)
      {
         try
         {
            return XDocument.Load(path, LoadOptions.SetLineInfo);
         }
         catch (XmlException error)
         {
            throw new ParseException("XML is not well-formed: " + error.Message);
         }
      }

      private static SfoParam ParseParam(XElement element)
      {
         string key = GetRequiredAttribute(element, "key");
         if (!KeyPattern.IsMatch(key))
            throw new ParseException(LocationOf(element) + "key '" + key +
               "' must be uppercase letters, digits, and underscores (e.g. TITLE_ID).");

         KeyDefaults defaults;
         bool wellKnown = WellKnownKeys.TryGetValue(key, out defaults);
         SfoDataType type = ResolveType(element, wellKnown, defaults);

         // the element's text is the value (a value= attribute would fight multi-line LICENSE text).
         string value = (element.Value ?? string.Empty).Trim();

         // int4 is always a fixed 4-byte slot; string types carry an explicit maxlength.
         if (type == SfoDataType.Int4)
            return new SfoParam(key, type, 4, EncodeInt(element, key, value));

         int maxLength = ResolveMaxLength(element, key, wellKnown, defaults);
         return new SfoParam(key, type, maxLength, EncodeString(element, key, type, value, maxLength));
      }

      // type: explicit type= wins, then the well-known default, then plain utf8.
      private static SfoDataType ResolveType(XElement element, bool wellKnown, KeyDefaults defaults)
      {
         string typeName = GetOptionalAttribute(element, "type");
         if (typeName == null)
            return wellKnown ? defaults.Type : SfoDataType.Utf8;

         SfoDataType type;
         if (!TypeNames.TryGetValue(typeName, out type))
            throw new ParseException(LocationOf(element) + "type '" + typeName +
               "' must be one of: utf8, utf8-special, int4.");
         return type;
      }

      // maxlength: explicit maxlength= wins, then the well-known default; otherwise it's required.
      private static int ResolveMaxLength(XElement element, string key, bool wellKnown, KeyDefaults defaults)
      {
         string raw = GetOptionalAttribute(element, "maxlength");
         if (raw != null)
         {
            int maxLength;
            if (!int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out maxLength) || maxLength <= 0)
               throw new ParseException(LocationOf(element) + "key '" + key +
                  "' maxlength '" + raw + "' must be a positive integer.");
            return maxLength;
         }
         if (wellKnown)
            return defaults.MaxLength;
         throw new ParseException(LocationOf(element) + "key '" + key +
            "' is not a well-known SFO key; maxlength attribute is required.");
      }

      // int4 value -> 4 little-endian bytes. accepts decimal (63) or hex (0x3F).
      private static byte[] EncodeInt(XElement element, string key, string value)
      {
         int parsed;
         if (!TryParseInt(value, out parsed))
            throw new ParseException(LocationOf(element) + "key '" + key +
               "' has type int4 but value '" + value + "' is not an integer.");
         return BitConverter.GetBytes(parsed);   // little-endian on our windows targets
      }

      // string value -> utf8 bytes, validated against the slot size.
      private static byte[] EncodeString(XElement element, string key, SfoDataType type, string value, int maxLength)
      {
         byte[] utf8 = Encoding.UTF8.GetBytes(value);

         if (type == SfoDataType.Utf8Special)   // no trailing null
         {
            if (utf8.Length > maxLength)
               throw new ParseException(LocationOf(element) + "key '" + key + "' value is " +
                  utf8.Length + " bytes but maxlength is " + maxLength + ".");
            return utf8;
         }

         // utf8: null-terminated, so the value plus its terminator must fit.
         if (utf8.Length + 1 > maxLength)
            throw new ParseException(LocationOf(element) + "key '" + key + "' value is " +
               (utf8.Length + 1) + " bytes (incl. null) but maxlength is " + maxLength + ".");
         var payload = new byte[utf8.Length + 1];   // trailing byte stays zero
         Array.Copy(utf8, payload, utf8.Length);
         return payload;
      }

      // accepts "10", "0x0A", "0x10" so int4 values can stay hex-readable in the xml.
      private static bool TryParseInt(string raw, out int value)
      {
         if (string.IsNullOrEmpty(raw)) { value = 0; return false; }

         if (raw.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
         {
            uint unsigned;
            if (uint.TryParse(raw.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out unsigned))
            {
               value = unchecked((int)unsigned);
               return true;
            }
            value = 0;
            return false;
         }
         return int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out value);
      }

      // ---- attribute and location helpers ----

      private static string GetRequiredAttribute(XElement element, string name)
      {
         XAttribute attribute = element.Attribute(name);
         if (attribute == null)
            throw new ParseException(LocationOf(element) + "<" + element.Name.LocalName +
               "> is missing required attribute '" + name + "'.");
         return attribute.Value;
      }

      private static string GetOptionalAttribute(XElement element, string name)
      {
         XAttribute attribute = element.Attribute(name);
         return attribute == null ? null : attribute.Value;
      }

      // "line N: " prefix for error messages, when line info is available.
      private static string LocationOf(XElement element)
      {
         var lineInfo = (IXmlLineInfo)element;
         return lineInfo.HasLineInfo() ? "line " + lineInfo.LineNumber + ": " : "";
      }
   }
}
