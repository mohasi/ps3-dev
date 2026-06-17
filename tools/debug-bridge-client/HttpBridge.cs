using System;
using System.Collections.Generic;
using System.Net;
using System.Text;
using System.Threading;

namespace DebugBridgeClient
{
   // local http server that proxies commands to the ps3 debug bridge
   // usage: GET http://localhost:8786/ping  ->  forwards "ping" to ps3, returns response
   //        GET http://localhost:8786/restart-ps3  ->  forwards "restart-ps3", etc.
   //        GET http://localhost:8786/status  ->  returns connection status (local only)
   public class HttpBridge
   {
      public const int Port = 8786;

      private const string UsageText =
         "debug-bridge-client http proxy\n\nusage: GET /<command>\nexample: /ping, /restart-ps3\n/status - connection status";

      // fired whenever a /capture request returns binary data, so the UI
      // can mirror http-driven captures onto the screen canvas.
      public delegate void CaptureHandler(int x, int y, int w, int h, byte[] argb);
      public event CaptureHandler CaptureReceived;

      private readonly Ps3Connection ps3;
      private readonly Action<string> log;
      private HttpListener listener;
      private Thread listenThread;
      private volatile bool running;

      public HttpBridge(Ps3Connection ps3, Action<string> log)
      {
         this.ps3 = ps3;
         this.log = log;
      }

      public void Start()
      {
         listener = new HttpListener();
         listener.Prefixes.Add("http://localhost:" + Port + "/");
         listener.Start();
         running = true;

         listenThread = new Thread(ListenLoop) { IsBackground = true };
         listenThread.Start();
      }

      public void Stop()
      {
         running = false;
         try { listener.Stop(); } catch { }
      }

      private void ListenLoop()
      {
         while (running)
         {
            try
            {
               HttpListenerContext ctx = listener.GetContext();
               ThreadPool.QueueUserWorkItem(HandleRequest, ctx);
            }
            catch
            {
               if (!running) break;
            }
         }
      }

      private void HandleRequest(object state)
      {
         HttpListenerContext ctx = (HttpListenerContext)state;
         HttpListenerResponse resp = ctx.Response;
         try
         {
            // a null body means the route already streamed its own response
            // (the binary endpoints write their payload straight to resp).
            string body = Route(ctx);
            if (body == null) return;

            byte[] buffer = Encoding.UTF8.GetBytes(body);
            resp.ContentType = "text/plain; charset=utf-8";
            resp.ContentLength64 = buffer.Length;
            resp.StatusCode = 200;
            resp.OutputStream.Write(buffer, 0, buffer.Length);
         }
         catch (Exception ex)
         {
            byte[] err = Encoding.UTF8.GetBytes("ERR " + ex.Message);
            resp.StatusCode = 500;
            resp.ContentLength64 = err.Length;
            resp.OutputStream.Write(err, 0, err.Length);
         }
         finally
         {
            resp.Close();
         }
      }

      // maps the url path to a handler. returns the text body to send, or
      // null when the handler already streamed a binary response to resp.
      private string Route(HttpListenerContext ctx)
      {
         string path = ctx.Request.Url.AbsolutePath.TrimStart('/');

         if (path.Length == 0) return UsageText;

         // /status reads a cached flag only — it never enters the sendLock.
         // a wedged in-flight command on a stalled bridge must not make
         // /status itself appear to time out, otherwise the operator can't
         // tell "host hung" from "ps3 hung".
         if (Is(path, "status")) return ps3.IsConnected ? "connected" : "disconnected";

         if (Is(path, "delete-file")) return DeleteFile(ctx);
         if (Is(path, "list-dir"))    return ListDir(ctx);
         if (Is(path, "read-mem"))    return ReadMem(ctx);
         if (Is(path, "capture"))     return Capture(ctx);
         if (Is(path, "stat-tree"))   return StatTree(ctx);
         if (Is(path, "pull-file"))   return PullFile(ctx);

         return Generic(ctx, path);
      }

      private static bool Is(string path, string name)
      {
         return path.Equals(name, StringComparison.OrdinalIgnoreCase);
      }

      // GET /delete-file?path=/dev_hdd0/foo.txt
      private string DeleteFile(HttpListenerContext ctx)
      {
         string filePath = QueryValue(ctx, "path");
         if (string.IsNullOrEmpty(filePath))
            return "ERR usage: /delete-file?path=<ps3-path>";
         return SendText("delete-file \"" + filePath + "\"");
      }

      // GET /list-dir?path=/dev_hdd0 — tab-separated listing, streamed as text.
      private string ListDir(HttpListenerContext ctx)
      {
         string dirPath = QueryValue(ctx, "path");
         if (string.IsNullOrEmpty(dirPath))
            return "ERR usage: /list-dir?path=<ps3-path>";
         byte[] payload;
         string err = WriteBinaryReply(ctx.Response, "list-dir \"" + dirPath + "\"", out payload, "text/plain; charset=utf-8");
         return payload != null ? null : err;
      }

      // GET /read-mem?<hexAddr>&<decLen> — raw bytes from ps3 vsh memory.
      // positional only, matches the bridge syntax.
      private string ReadMem(HttpListenerContext ctx)
      {
         string raw = (ctx.Request.Url.Query ?? "").TrimStart('?');
         string[] parts = raw.Split('&');
         if (parts.Length < 2 || string.IsNullOrEmpty(parts[0]) || string.IsNullOrEmpty(parts[1]))
            return "ERR usage: /read-mem?<hexAddr>&<decLen>";
         string cmd = "read-mem " + Uri.UnescapeDataString(parts[0]) + " " + Uri.UnescapeDataString(parts[1]);
         byte[] payload;
         string err = WriteBinaryReply(ctx.Response, cmd, out payload, "application/octet-stream");
         return payload != null ? null : err;
      }

      // GET /capture?x=X&y=Y&w=W&h=H — raw ARGB8888 bytes (vram byte order
      // A,R,G,B per pixel), w*h*4 in length. also mirrors the capture to the
      // screen canvas so any /capture caller (ui, curl, scripts) shows up live.
      private string Capture(HttpListenerContext ctx)
      {
         var q = ParseQuery(ctx.Request.Url.Query);
         int x = IntArg(q, "x", 0), y = IntArg(q, "y", 0);
         int w = IntArg(q, "w", 1), h = IntArg(q, "h", 1);
         byte[] argb;
         string err = WriteBinaryReply(ctx.Response, "capture " + x + " " + y + " " + w + " " + h, out argb);
         if (argb == null) return err;
         CaptureHandler handler = CaptureReceived;
         if (handler != null && argb.Length == w * h * 4) handler(x, y, w, h, argb);
         return null;
      }

      // GET /stat-tree?root=/dev_hdd0 — kicks off a recursive sha1'd snapshot
      // on the ps3 written to /dev_hdd0/tmp/stat-tree.txt. caller pulls the
      // file separately via /pull-file. stat-tree is the only command that
      // legitimately runs for minutes, so its long timeout stays inline here
      // rather than leaking into SendText.
      private string StatTree(HttpListenerContext ctx)
      {
         string root = QueryValue(ctx, "root");
         if (string.IsNullOrEmpty(root))
            return "ERR usage: /stat-tree?root=<ps3-path>";
         string cmd = "stat-tree \"" + root + "\"";
         log("http -> " + cmd);
         Ps3Reply r = ps3.SendCommand(cmd, null, MainWindow.StatTreeTimeoutMs);
         string body = (r.Ok ? "OK" : "ERR") + (r.Payload.Length > 0 ? " " + r.AsText() : "");
         log("ps3 -> " + body);
         return body;
      }

      // GET /pull-file?path=/dev_hdd0/tmp/dbg.txt[&offset=N&length=N][&text=1]
      private string PullFile(HttpListenerContext ctx)
      {
         var q = ParseQuery(ctx.Request.Url.Query);
         string filePath = q.ContainsKey("path") ? q["path"] : null;
         if (string.IsNullOrEmpty(filePath))
            return "ERR usage: /pull-file?path=<ps3-path>[&offset=N&length=N][&text=1]";
         string cmd = "pull-file \"" + filePath + "\"";
         if (q.ContainsKey("offset") || q.ContainsKey("length"))
         {
            string offset = q.ContainsKey("offset") ? q["offset"] : "0";
            string length = q.ContainsKey("length") ? q["length"] : "0";
            cmd = cmd + " " + offset + " " + length;
         }
         bool asText = q.ContainsKey("text") && q["text"] == "1";
         byte[] payload;
         string err = WriteBinaryReply(ctx.Response, cmd, out payload, asText ? "text/plain; charset=utf-8" : "application/octet-stream");
         return payload != null ? null : err;
      }

      // generic passthrough for any other command. POST bodies are forwarded
      // as the binary payload (used by uploads like vsh-plugin-install); query
      // args are forwarded positionally in url order.
      private string Generic(HttpListenerContext ctx, string command)
      {
         HttpListenerRequest req = ctx.Request;
         byte[] payload = ReadPostBody(req);

         if (!string.IsNullOrEmpty(req.Url.Query))
            command += PositionalArgs(req.Url.Query);
         if (payload != null)
            command = command + " " + payload.Length;

         return SendText(command, payload);
      }

      private static byte[] ReadPostBody(HttpListenerRequest req)
      {
         if (!string.Equals(req.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase) || req.ContentLength64 <= 0)
            return null;
         byte[] payload = new byte[req.ContentLength64];
         int off = 0;
         while (off < payload.Length)
         {
            int n = req.InputStream.Read(payload, off, payload.Length - off);
            if (n <= 0) break;
            off += n;
         }
         return payload;
      }

      // accept either positional ("?foo&bar") or named ("?name=foo&size=bar")
      // query params — both forward the *values* in url order as positional args.
      private static string PositionalArgs(string query)
      {
         var sb = new StringBuilder();
         foreach (string pair in query.TrimStart('?').Split('&'))
         {
            if (string.IsNullOrEmpty(pair)) continue;
            int eq = pair.IndexOf('=');
            string val = eq < 0 ? pair : pair.Substring(eq + 1);
            sb.Append(' ').Append(Uri.UnescapeDataString(val));
         }
         return sb.ToString();
      }

      // text-mode: log the command, decode the reply payload as ascii.
      // multi-line payloads (e.g. module-info) get one log line per record,
      // continuation lines indented under the "ps3 -> " prefix. The UI
      // batches appends, so volume is fine.
      private string SendText(string command, byte[] upload = null)
      {
         log("http -> " + command + (upload != null ? " (" + upload.Length + " bytes)" : ""));
         Ps3Reply r = ps3.SendCommand(command, upload);
         string body = (r.Ok ? "OK" : "ERR") + (r.Payload.Length > 0 ? " " + r.AsText() : "");
         string[] lines = body.Split('\n');
         log("ps3 -> " + lines[0]);
         for (int i = 1; i < lines.Length; i++)
            if (lines[i].Length > 0) log("       " + lines[i]);
         return body;
      }

      // binary-mode: send command, write the payload to resp on success.
      // returns null on success (payload was written) or the ERR text on failure.
      private string WriteBinaryReply(HttpListenerResponse resp, string command, out byte[] payload, string contentType = "application/octet-stream")
      {
         log("http -> " + command);
         Ps3Reply r = ps3.SendCommand(command);
         if (!r.Ok)
         {
            payload = null;
            string text = "ERR " + r.AsText();
            log("ps3 -> " + text);
            return text;
         }
         log("ps3 -> OK " + r.Payload.Length);
         payload = r.Payload;
         resp.ContentType = contentType;
         resp.ContentLength64 = payload.Length;
         resp.StatusCode = 200;
         resp.OutputStream.Write(payload, 0, payload.Length);
         return null;
      }

      // a single named query value, or null if absent.
      private static string QueryValue(HttpListenerContext ctx, string key)
      {
         var q = ParseQuery(ctx.Request.Url.Query);
         return q.ContainsKey(key) ? q[key] : null;
      }

      private static int IntArg(Dictionary<string, string> q, string key, int fallback)
      {
         int v;
         return q.ContainsKey(key) && int.TryParse(q[key], out v) ? v : fallback;
      }

      private static Dictionary<string, string> ParseQuery(string query)
      {
         var dict = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
         if (string.IsNullOrEmpty(query)) return dict;
         foreach (string pair in query.TrimStart('?').Split('&'))
         {
            if (string.IsNullOrEmpty(pair)) continue;
            int eq = pair.IndexOf('=');
            string k = eq < 0 ? pair : pair.Substring(0, eq);
            string v = eq < 0 ? "" : Uri.UnescapeDataString(pair.Substring(eq + 1));
            dict[k] = v;
         }
         return dict;
      }
   }
}
