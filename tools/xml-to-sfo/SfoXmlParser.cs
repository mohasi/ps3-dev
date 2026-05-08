using System;
using System.Collections.Generic;
using System.Globalization;
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
        // Matches the PSF key naming convention used by Sony (uppercase letters,
        // digits, underscores, e.g. PS3_SYSTEM_VER). Keep this strict — the
        // parser would happily accept anything, but it would just confuse the
        // PS3 if you fed it surprises.
        private static readonly Regex KeyRegex = new Regex(@"^[A-Z][A-Z0-9_]*$", RegexOptions.Compiled);

        private static readonly Dictionary<string, SfoDataType> TypeMap =
            new Dictionary<string, SfoDataType>(StringComparer.Ordinal)
        {
            { "utf8",         SfoDataType.Utf8 },
            { "utf8-special", SfoDataType.Utf8Special },
            { "int4",         SfoDataType.Int4 }
        };

        // well-known SFO keys with their default type and maxlength so the
        // xml doesn't need to repeat them on every entry. values are from
        // psdevwiki PARAM.SFO and sony sdk samples.
        private struct KeyDefaults
        {
            public SfoDataType Type;
            public int MaxLength;
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
            XDocument doc;
            try
            {
                doc = XDocument.Load(path, LoadOptions.SetLineInfo);
            }
            catch (XmlException ex)
            {
                throw new ParseException("XML is not well-formed: " + ex.Message);
            }

            var root = doc.Root;
            if (root == null || root.Name.LocalName != "paramsfo")
                throw new ParseException("Root element must be <paramsfo>.");

            var model = new SfoModel();
            var seenKeys = new HashSet<string>(StringComparer.Ordinal);

            foreach (var el in root.Elements())
            {
                if (el.Name.LocalName != "param")
                    throw new ParseException(At(el) + "unexpected element <" + el.Name.LocalName +
                        ">; only <param> is allowed inside <paramsfo>.");

                var p = ParseParam(el);

                if (!seenKeys.Add(p.Key))
                    throw new ParseException(At(el) + "duplicate key '" + p.Key + "'.");

                model.Params.Add(p);
            }

            if (model.Params.Count == 0)
                throw new ParseException("<paramsfo> contains no <param> entries.");

            return model;
        }

        private static SfoParam ParseParam(XElement el)
        {
            string key = RequireAttr(el, "key");
            if (!KeyRegex.IsMatch(key))
                throw new ParseException(At(el) + "key '" + key +
                    "' must be uppercase letters, digits, and underscores (e.g. TITLE_ID).");

            // look up well-known defaults for this key
            KeyDefaults defaults;
            bool hasDefaults = WellKnownKeys.TryGetValue(key, out defaults);

            // type: explicit attr wins, then well-known default, then utf8
            string typeStr = OptAttr(el, "type");
            SfoDataType type;
            if (typeStr != null)
            {
                if (!TypeMap.TryGetValue(typeStr, out type))
                    throw new ParseException(At(el) + "type '" + typeStr +
                        "' must be one of: utf8, utf8-special, int4.");
            }
            else if (hasDefaults)
            {
                type = defaults.Type;
            }
            else
            {
                type = SfoDataType.Utf8;
            }

            // element text is the value (xml-to-c uses attributes, but a value
            // attribute fights with multi-line LICENSE text; element text reads
            // more naturally for SFO).
            string raw = (el.Value ?? string.Empty).Trim();

            int maxLength;
            byte[] payload;

            if (type == SfoDataType.Int4)
            {
                maxLength = 4;
                int v;
                if (!TryParseIntFlexible(raw, out v))
                    throw new ParseException(At(el) + "key '" + key +
                        "' has type int4 but value '" + raw + "' is not an integer.");
                payload = new byte[4];
                payload[0] = (byte)(v & 0xFF);
                payload[1] = (byte)((v >> 8) & 0xFF);
                payload[2] = (byte)((v >> 16) & 0xFF);
                payload[3] = (byte)((v >> 24) & 0xFF);
            }
            else
            {
                string maxLenStr = OptAttr(el, "maxlength");
                if (maxLenStr != null)
                {
                    if (!int.TryParse(maxLenStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out maxLength) || maxLength <= 0)
                        throw new ParseException(At(el) + "key '" + key +
                            "' maxlength '" + maxLenStr + "' must be a positive integer.");
                }
                else if (hasDefaults)
                {
                    maxLength = defaults.MaxLength;
                }
                else
                {
                    throw new ParseException(At(el) + "key '" + key +
                        "' is not a well-known SFO key; maxlength attribute is required.");
                }

                byte[] utf8 = System.Text.Encoding.UTF8.GetBytes(raw);

                if (type == SfoDataType.Utf8)
                {
                    // null-terminated: bytes + 1 must fit in maxLength
                    if (utf8.Length + 1 > maxLength)
                        throw new ParseException(At(el) + "key '" + key + "' value is " +
                            (utf8.Length + 1) + " bytes (incl. null) but maxlength is " + maxLength + ".");
                    payload = new byte[utf8.Length + 1];
                    Array.Copy(utf8, payload, utf8.Length);
                    payload[utf8.Length] = 0; // explicit null
                }
                else // Utf8Special
                {
                    if (utf8.Length > maxLength)
                        throw new ParseException(At(el) + "key '" + key + "' value is " +
                            utf8.Length + " bytes but maxlength is " + maxLength + ".");
                    payload = utf8;
                }
            }

            return new SfoParam
            {
                Key = key,
                Type = type,
                MaxLength = maxLength,
                Payload = payload
            };
        }

        // Accept "10", "0x0A", "0x10". Used for int4 values so RESOLUTION etc
        // can be hex-readable in the XML.
        private static bool TryParseIntFlexible(string raw, out int value)
        {
            if (string.IsNullOrEmpty(raw)) { value = 0; return false; }
            if (raw.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                uint u;
                if (uint.TryParse(raw.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out u))
                {
                    value = unchecked((int)u);
                    return true;
                }
                value = 0;
                return false;
            }
            return int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out value);
        }

        // ---- attribute helpers (mirrors xml-to-c's style) ----

        private static string RequireAttr(XElement el, string name)
        {
            var a = el.Attribute(name);
            if (a == null)
                throw new ParseException(At(el) + "<" + el.Name.LocalName +
                    "> is missing required attribute '" + name + "'.");
            return a.Value;
        }

        private static string OptAttr(XElement el, string name)
        {
            var a = el.Attribute(name);
            return a == null ? null : a.Value;
        }

        private static string At(XElement el)
        {
            var li = (IXmlLineInfo)el;
            return li.HasLineInfo()
                ? "line " + li.LineNumber + ": "
                : "";
        }
    }
}
