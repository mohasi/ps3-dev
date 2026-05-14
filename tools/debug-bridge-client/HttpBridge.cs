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
                else
                {
                    // build command string: path + query params as arguments
                    string command = path;
                    if (!string.IsNullOrEmpty(req.Url.Query))
                    {
                        string args = Uri.UnescapeDataString(req.Url.Query.TrimStart('?'));
                        command = command + " " + args;
                    }

                    log("http -> " + command);
                    body = ps3.SendCommand(command);
                    log("ps3 -> " + body);
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
    }
}
