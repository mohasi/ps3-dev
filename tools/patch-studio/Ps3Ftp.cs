using System;
using System.Collections.Generic;
using System.IO;
using System.Net;

namespace PatchStudio
{
   // anonymous FTP to the console (as simple-ftp / webMAN serve), for Download Dump and Deploy.
   // the console's IP comes from settings.txt (ps3ip). every call is one short-lived request.
   public static class Ps3Ftp
   {
      // names of the files in a remote directory (leaf names only). doubles as the reachability
      // check: if it answers, the console is there. returns false with a message on any failure.
      public static bool TryListFiles(string ip, string remoteDir, out List<string> files, out string error)
      {
         files = new List<string>();
         error = "";
         try
         {
            FtpWebRequest request = newRequest(ip, remoteDir, WebRequestMethods.Ftp.ListDirectory);
            request.Timeout = 8000;
            using (WebResponse response = request.GetResponse())
            using (var reader = new StreamReader(response.GetResponseStream()))
            {
               string line;
               while ((line = reader.ReadLine()) != null)
               {
                  string name = Path.GetFileName(line.Trim());
                  if (name != "") files.Add(name);
               }
            }
            return true;
         }
         catch (Exception exception) { error = exception.Message; return false; }
      }

      public static void DownloadFile(string ip, string remotePath, string localPath)
      {
         FtpWebRequest request = newRequest(ip, remotePath, WebRequestMethods.Ftp.DownloadFile);
         request.UseBinary = true;
         request.Timeout = 60000;
         request.ReadWriteTimeout = 60000;
         using (WebResponse response = request.GetResponse())
         using (Stream remote = response.GetResponseStream())
         using (Stream local = File.Create(localPath))
            remote.CopyTo(local);
      }

      public static void UploadFile(string ip, string remotePath, byte[] bytes)
      {
         FtpWebRequest request = newRequest(ip, remotePath, WebRequestMethods.Ftp.UploadFile);
         request.UseBinary = true;
         request.Timeout = 120000;
         request.ReadWriteTimeout = 120000;
         request.ContentLength = bytes.Length;
         using (Stream stream = request.GetRequestStream()) stream.Write(bytes, 0, bytes.Length);
         using (request.GetResponse()) { }
      }

      // create one directory level; a "550 already exists" is success for our purposes. FTP mkdir is
      // not recursive, so the caller makes each parent level in turn.
      public static void MakeDir(string ip, string remoteDir)
      {
         try
         {
            FtpWebRequest request = newRequest(ip, remoteDir, WebRequestMethods.Ftp.MakeDirectory);
            request.Timeout = 8000;
            using (request.GetResponse()) { }
         }
         catch (WebException) { }   // already exists (or a benign server refusal) — parents/uploads still proceed
      }

      private static FtpWebRequest newRequest(string ip, string remotePath, string method)
      {
         var request = (FtpWebRequest)WebRequest.Create("ftp://" + ip + remotePath);
         request.Method = method;
         request.KeepAlive = false;
         request.UsePassive = true;
         request.Credentials = new NetworkCredential("anonymous", "anonymous");
         return request;
      }
   }
}
