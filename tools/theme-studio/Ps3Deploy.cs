using System;
using System.Collections.Generic;
using System.IO;
using System.Net;

namespace ThemeStudio
{
   // uploads a built theme to /dev_hdd0/theme over FTP (anonymous, as simple-ftp serves), where
   // the console lists it under Theme Settings with no install step. the console needs an FTP
   // server running and its address in settings.txt.
   public static class Ps3Deploy
   {
      public const string ThemeDir = "/dev_hdd0/theme";

      // the console lists no more than this many themes. the extras are not rejected, they simply
      // never appear, so a deploy that would push past it is stopped here instead.
      public const int MaxThemes = 100;

      // the themes already on the console. doubles as the reachability check: if this answers,
      // the console is there.
      public static bool TryListThemes(string ip, out List<string> themes, out string error)
      {
         themes = new List<string>();
         error = "";
         try {
            FtpWebRequest request = newRequest(ip, "", WebRequestMethods.Ftp.ListDirectory);
            request.Timeout = 8000;
            using (WebResponse response = request.GetResponse())
            using (var reader = new StreamReader(response.GetResponseStream())) {
               string line;
               while ((line = reader.ReadLine()) != null) {
                  string name = Path.GetFileName(line.Trim());
                  if (name.EndsWith(".p3t", StringComparison.OrdinalIgnoreCase)) themes.Add(name);
               }
            }
            return true;
         } catch (Exception exception) {
            error = exception.Message;
            return false;
         }
      }

      public static bool HoldsTheme(IEnumerable<string> themes, string fileName)
      {
         foreach (string theme in themes)
            if (string.Equals(theme, fileName, StringComparison.OrdinalIgnoreCase)) return true;
         return false;
      }

      public static void Upload(string ip, string localP3tPath)
      {
         FtpWebRequest request = newRequest(ip, Path.GetFileName(localP3tPath), WebRequestMethods.Ftp.UploadFile);
         request.UseBinary = true;
         request.Timeout = 120000;
         request.ReadWriteTimeout = 120000;

         byte[] bytes = File.ReadAllBytes(localP3tPath);
         request.ContentLength = bytes.Length;
         using (Stream stream = request.GetRequestStream()) stream.Write(bytes, 0, bytes.Length);
         using (request.GetResponse()) { }
      }

      private static FtpWebRequest newRequest(string ip, string fileName, string method)
      {
         string path = ThemeDir + (fileName == "" ? "" : "/" + fileName);
         var request = (FtpWebRequest)WebRequest.Create("ftp://" + ip + path);
         request.Method = method;
         request.KeepAlive = false;
         request.UsePassive = true;
         request.Credentials = new NetworkCredential("anonymous", "anonymous");
         return request;
      }
   }
}
