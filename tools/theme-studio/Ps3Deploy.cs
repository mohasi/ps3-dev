using System;
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

      // true if the console answers a listing of the theme folder within a short timeout
      public static bool CanReach(string ip, out string error)
      {
         error = "";
         try {
            FtpWebRequest request = newRequest(ip, "", WebRequestMethods.Ftp.ListDirectory);
            request.Timeout = 4000;
            using (request.GetResponse()) { }
            return true;
         } catch (Exception exception) {
            error = exception.Message;
            return false;
         }
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
