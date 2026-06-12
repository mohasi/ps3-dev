using System;
using System.Globalization;
using System.IO;

namespace DebugBridgeClient
{
    // shared helpers for the line-oriented NID json files (nid_names.json,
    // nid_names_local.json, nid_protos.json). they all sit beside the exe or
    // under nid-dump/, and all key on a "0xNNNNNNNN" hex string. keeping the
    // file lookup and key parsing here stops NidNames and NidProtos drifting
    // out of sync (they used to carry private copies of both).
    internal static class NidJson
    {
        // search order, relative to the exe directory:
        //   ./<file>                next to the exe (deployed copy)
        //   ./nid-dump/<file>       exe sits in tools/, json in tools/nid-dump/
        //   ../nid-dump/<file>      exe sits in tools/<peer>/, json in tools/nid-dump/
        //   ../../nid-dump/<file>   exe sits in tools/debug-bridge-client/bin/
        public static string FindFile(string fileName)
        {
            string exeDir = AppDomain.CurrentDomain.BaseDirectory ?? ".";
            string[] candidates = {
                Path.Combine(exeDir, fileName),
                Path.Combine(exeDir, @"nid-dump\" + fileName),
                Path.Combine(exeDir, @"..\nid-dump\" + fileName),
                Path.Combine(exeDir, @"..\..\nid-dump\" + fileName),
            };
            foreach (string candidate in candidates) {
                try { if (File.Exists(candidate)) return Path.GetFullPath(candidate); }
                catch { }
            }
            return null;
        }

        // parses a "0xNNNNNNNN" (or bare hex) json key into a nid.
        public static bool TryParseKey(string token, out uint nid)
        {
            nid = 0;
            if (string.IsNullOrEmpty(token)) return false;
            string hex = (token.Length > 2 && (token[1] == 'x' || token[1] == 'X')) ? token.Substring(2) : token;
            return uint.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out nid);
        }
    }
}
