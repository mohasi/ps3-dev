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
                    body = ps3.IsConnected ? "connected" : "disconnected";
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
                    {
                        body = "ERR usage: /delete-file?path=<ps3-path>";
                    }
                    else
                    {
                        string cmd = "delete-file \"" + filePath + "\"";
                        log("http -> " + cmd);
                        string[] lines = ps3.SendCommand(cmd);
                        body = string.Join("\n", lines);
                        foreach (string line in lines) log("ps3 -> " + line);
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
                        log("http -> " + cmd);
                        var dl = ps3.Download(cmd);
                        log("ps3 -> " + dl.Status);
                        if (dl.Data == null)
                        {
                            body = dl.Status;
                        }
                        else
                        {
                            bool asText = q.ContainsKey("text") && q["text"] == "1";
                            resp.ContentType = asText ? "text/plain; charset=utf-8" : "application/octet-stream";
                            resp.ContentLength64 = dl.Data.Length;
                            resp.StatusCode = 200;
                            resp.OutputStream.Write(dl.Data, 0, dl.Data.Length);
                            return;
                        }
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
                    if (payload != null)
                    {
                        command = command + " " + payload.Length;
                    }

                    log("http -> " + command + (payload != null ? " (" + payload.Length + " bytes)" : ""));
                    string[] lines = payload != null
                        ? ps3.SendCommandWithPayload(command, payload)
                        : ps3.SendCommand(command);
                    body = string.Join("\n", lines);
                    foreach (string line in lines) log("ps3 -> " + line);
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
