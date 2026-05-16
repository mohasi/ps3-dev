using System;
using System.Configuration;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace DebugBridgeClient
{
    // persistent duplex tcp connection to the ps3 simple-debug-bridge plugin
    // (port 8785). a background thread keeps one socket open and reconnects
    // on drop. all callers go through SendCommand, which writes a framed
    // request and reads a framed reply on the same socket.
    public class Ps3Connection
    {
        public const string DefaultHost = "10.0.0.2";
        public const int Port = 8785;
        public const int ReconnectDelayMs = 3000;

        public event EventHandler Connected;
        public event EventHandler Disconnected;

        private readonly object sendLock = new object();
        private volatile bool running;
        private volatile bool connected;
        private string host;
        private TcpClient    tcp;
        private NetworkStream stream;

        public string Host { get { return host; } }
        public bool IsConnected { get { return connected; } }

        public Ps3Connection()
        {
            string configured = ConfigurationManager.AppSettings["Ps3IpAddress"];
            host = string.IsNullOrEmpty(configured) ? DefaultHost : configured;
        }

        public void StartAutoConnect()
        {
            running = true;
            Thread t = new Thread(AutoConnectLoop);
            t.IsBackground = true;
            t.Start();
        }

        public void Disconnect()
        {
            running = false;
            DropSocket();
        }

        // send one command and return the next framed reply. an optional raw
        // upload payload is appended after the newline (for save-file etc).
        // on transport error returns an ERR reply and drops the socket so the
        // auto-connect loop reconnects on the next ping.
        public Ps3Reply SendCommand(string command, byte[] upload = null)
        {
            lock (sendLock)
            {
                if (stream == null)
                    return Ps3Reply.Error("not connected");
                try
                {
                    byte[] cmd = Encoding.ASCII.GetBytes(command + "\n");
                    stream.Write(cmd, 0, cmd.Length);
                    if (upload != null && upload.Length > 0)
                        stream.Write(upload, 0, upload.Length);

                    return ReadFramedReply();
                }
                catch (Exception ex)
                {
                    DropSocket();
                    return Ps3Reply.Error(ex.Message);
                }
            }
        }

        // read "<STATUS> <n>\n" then exactly n bytes.
        private Ps3Reply ReadFramedReply()
        {
            string header = ReadHeaderLine();
            int sp = header.IndexOf(' ');
            if (sp < 0) throw new IOException("malformed reply header: " + header);
            string status = header.Substring(0, sp);
            int n;
            if (!int.TryParse(header.Substring(sp + 1), out n) || n < 0)
                throw new IOException("malformed reply length: " + header);

            byte[] payload = new byte[n];
            int off = 0;
            while (off < n)
            {
                int got = stream.Read(payload, off, n - off);
                if (got <= 0) throw new IOException("connection closed mid-payload");
                off += got;
            }
            return new Ps3Reply(status == "OK", payload);
        }

        private string ReadHeaderLine()
        {
            var sb = new StringBuilder(32);
            for (;;)
            {
                int b = stream.ReadByte();
                if (b < 0) throw new IOException("connection closed before header");
                if (b == '\n') return sb.ToString();
                if (b != '\r') sb.Append((char)b);
                if (sb.Length > 64) throw new IOException("header too long");
            }
        }

        private void DropSocket()
        {
            try { if (stream != null) stream.Close(); } catch { }
            try { if (tcp    != null) tcp.Close();    } catch { }
            stream = null;
            tcp = null;
            if (connected)
            {
                connected = false;
                EventHandler evt = Disconnected;
                if (evt != null) evt(this, EventArgs.Empty);
            }
        }

        private bool TryConnect()
        {
            try
            {
                var t = new TcpClient();
                IAsyncResult ar = t.BeginConnect(host, Port, null, null);
                if (!ar.AsyncWaitHandle.WaitOne(3000, false))
                {
                    t.Close();
                    return false;
                }
                t.EndConnect(ar);
                t.NoDelay = true;
                t.ReceiveTimeout = 60000;
                t.SendTimeout    = 60000;
                lock (sendLock)
                {
                    tcp    = t;
                    stream = t.GetStream();
                }
                return true;
            }
            catch { return false; }
        }

        private void AutoConnectLoop()
        {
            while (running)
            {
                if (!connected)
                {
                    if (TryConnect())
                    {
                        // probe with a ping to confirm the socket is live.
                        Ps3Reply r = SendCommand("ping");
                        if (r.Ok)
                        {
                            connected = true;
                            EventHandler evt = Connected;
                            if (evt != null) evt(this, EventArgs.Empty);
                        }
                        else
                        {
                            DropSocket();
                        }
                    }
                }
                else
                {
                    // periodic liveness check; SendCommand drops on failure.
                    SendCommand("ping");
                }
                Thread.Sleep(ReconnectDelayMs);
            }
        }
    }
}
