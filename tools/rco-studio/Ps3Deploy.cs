using System;
using System.IO;
using System.Net;

namespace RcoStudio
{
   // uploads compiled RCOs to the PS3's dev_blind over FTP (anonymous, as simple-ftp serves).
   // dev_blind is the writable mirror of dev_flash, so this replaces the live XMB resources --
   // a restart picks them up. no backup is taken: if a bad RCO stops the XMB, the FTP server
   // (itself a VSH plugin) won't load either, so a backup pushed back over FTP couldn't help.
   public static class Ps3Deploy
   {
      private const string ResourceDir = "/dev_blind/vsh/resource";

      // true if the console answers an FTP listing of the resource dir within a short timeout
      public static bool CanReach(string ip, out string error)
      {
         error = "";
         try
         {
            FtpWebRequest request = NewRequest(ip, "", WebRequestMethods.Ftp.ListDirectory);
            request.Timeout = 4000;
            using (request.GetResponse()) { }
            return true;
         }
         catch (Exception exception) { error = exception.Message; return false; }
      }

      // uploads one compiled rco to /dev_blind/vsh/resource/<name>.rco, overwriting it
      public static void Upload(string ip, string localRco, string rcoName)
      {
         FtpWebRequest request = NewRequest(ip, rcoName + ".rco", WebRequestMethods.Ftp.UploadFile);
         request.UseBinary = true;
         byte[] bytes = File.ReadAllBytes(localRco);
         request.ContentLength = bytes.Length;
         using (Stream stream = request.GetRequestStream()) stream.Write(bytes, 0, bytes.Length);
         using (request.GetResponse()) { }
      }

      private static FtpWebRequest NewRequest(string ip, string fileName, string method)
      {
         string path = ResourceDir + (fileName == "" ? "" : "/" + fileName);
         var request = (FtpWebRequest)WebRequest.Create("ftp://" + ip + path);
         request.Method = method;
         request.KeepAlive = false;
         request.UsePassive = true;
         request.Credentials = new NetworkCredential("anonymous", "anonymous");
         return request;
      }
   }
}
