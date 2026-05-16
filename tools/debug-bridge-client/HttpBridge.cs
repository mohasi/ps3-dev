using System;
using System.IO;
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

            listenThread = new Thread(ListenLoop);
            listenThread.IsBackground = true;
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
            HttpListenerRequest req = ctx.Request;
            HttpListenerResponse resp = ctx.Response;

            try
            {
                // strip leading slash to get the command
                string path = req.Url.AbsolutePath.TrimStart('/');

                string body;
                if (string.Equals(path, "status", StringComparison.OrdinalIgnoreCase))
                {
                    // live probe rather than the cached flag — callers (e.g.
                    // deploy.ps1) need to know the bridge is *actually* taking
                    // commands right now, not that it was up 3s ago.
                    body = ps3.SendCommand("ping").Ok ? "connected" : "disconnected";
                }
                else if (string.IsNullOrEmpty(path))
                {
                    body = "debug-bridge-client http proxy\n\nusage: GET /<command>\nexample: /ping, /restart-ps3\n/status - connection status";
                }
                else if (path.Equals("delete-file", StringComparison.OrdinalIgnoreCase))
                {
                    // GET /delete-file?path=/dev_hdd0/foo.txt
                    var q = ParseQuery(req.Url.Query);
                    string filePath = q.ContainsKey("path") ? q["path"] : null;
                    if (string.IsNullOrEmpty(filePath))
                        body = "ERR usage: /delete-file?path=<ps3-path>";
                    else
                        body = SendText("delete-file \"" + filePath + "\"");
                }
                else if (path.Equals("capture", StringComparison.OrdinalIgnoreCase))
                {
                    // GET /capture?x=X&y=Y&w=W&h=H — returns raw ARGB8888 bytes
                    // (vram byte order: A,R,G,B per pixel), w*h*4 in length.
                    // also mirrors the capture to the screen canvas so any
                    // /capture caller (ui, curl, scripts) shows up live.
                    var q = ParseQuery(req.Url.Query);
                    int x = IntArg(q, "x", 0), y = IntArg(q, "y", 0);
                    int w = IntArg(q, "w", 1), h = IntArg(q, "h", 1);
                    byte[] argb;
                    body = WriteBinaryReply(resp, "capture " + x + " " + y + " " + w + " " + h, out argb);
                    if (argb != null)
                    {
                        var handler = CaptureReceived;
                        if (handler != null && argb.Length == w * h * 4) handler(x, y, w, h, argb);
                        return;
                    }
                }
                else if (path.Equals("get-file", StringComparison.OrdinalIgnoreCase))
                {
                    // GET /get-file?path=/dev_hdd0/tmp/dbg.txt[&offset=N&length=N][&text=1]
                    var q = ParseQuery(req.Url.Query);
                    string filePath = q.ContainsKey("path") ? q["path"] : null;
                    if (string.IsNullOrEmpty(filePath))
                    {
                        body = "ERR usage: /get-file?path=<ps3-path>[&offset=N&length=N][&text=1]";
                    }
                    else
                    {
                        string cmd = "get-file \"" + filePath + "\"";
                        if (q.ContainsKey("offset") || q.ContainsKey("length"))
                        {
                            string o = q.ContainsKey("offset") ? q["offset"] : "0";
                            string l = q.ContainsKey("length") ? q["length"] : "0";
                            cmd = cmd + " " + o + " " + l;
                        }
                        bool asText = q.ContainsKey("text") && q["text"] == "1";
                        byte[] payload;
                        body = WriteBinaryReply(resp, cmd, out payload, asText ? "text/plain; charset=utf-8" : "application/octet-stream");
                        if (payload != null) return;
                    }
                }
                else
                {
                    // POST bodies are forwarded as the binary payload (used by
                    // upload commands like vsh-plugin-install).
                    string command = path;
                    byte[] payload = null;
                    if (string.Equals(req.HttpMethod, "POST", StringComparison.OrdinalIgnoreCase) &&
                        req.ContentLength64 > 0)
                    {
                        payload = new byte[req.ContentLength64];
                        int off = 0;
                        while (off < payload.Length)
                        {
                            int n = req.InputStream.Read(payload, off, payload.Length - off);
                            if (n <= 0) break;
                            off += n;
                        }
                    }

                    if (!string.IsNullOrEmpty(req.Url.Query))
                    {
                        // accept either positional ("?foo&bar") or named
                        // ("?name=foo&size=bar") query params — both forward
                        // the *values* in URL order as positional command args.
                        string raw = req.Url.Query.TrimStart('?');
                        var sb = new StringBuilder();
                        foreach (string pair in raw.Split('&'))
                        {
                            if (string.IsNullOrEmpty(pair)) continue;
                            int eq = pair.IndexOf('=');
                            string val = eq < 0 ? pair : pair.Substring(eq + 1);
                            sb.Append(' ').Append(Uri.UnescapeDataString(val));
                        }
                        command = command + sb.ToString();
                    }
                    if (payload != null) command = command + " " + payload.Length;

                    body = SendText(command, payload);
                }

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

        // text-mode: log the command, decode the reply payload as ascii.
        // multi-line payloads (e.g. vsh-plugin-list) get one log line per
        // record, with continuation lines indented under the "ps3 -> " prefix.
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

        private static int IntArg(System.Collections.Generic.Dictionary<string, string> q, string key, int fallback)
        {
            int v;
            return q.ContainsKey(key) && int.TryParse(q[key], out v) ? v : fallback;
        }

        private static System.Collections.Generic.Dictionary<string, string> ParseQuery(string query)
        {
            var dict = new System.Collections.Generic.Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
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
