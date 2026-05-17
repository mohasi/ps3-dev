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
    // on drop. a dedicated reader thread demultiplexes the wire into either
    //   - OK/ERR replies (handed to the in-flight SendCommand caller), or
    //   - LOG frames (raised on LogReceived for the UI).
    public class Ps3Connection
    {
        public const string DefaultHost = "10.0.0.2";
        public const int Port = 8785;
        public const int ReconnectDelayMs = 3000;

        public event EventHandler Connected;
        public event EventHandler Disconnected;
        public event Action<string> LogReceived;

        private readonly object sendLock  = new object();   // one request at a time
        private readonly object replyLock = new object();
        private readonly ManualResetEvent replyReady = new ManualResetEvent(false);
        private Ps3Reply pendingReply;

        private volatile bool running;
        private volatile bool connected;
        private string host;
        private TcpClient    tcp;
        private NetworkStream stream;
        private Thread readerThread;

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

        // send one command and return the next OK/ERR reply. LOG frames are
        // routed to LogReceived by the reader thread and do not satisfy this
        // call. on transport error returns an ERR reply and drops the socket
        // so the auto-connect loop reconnects on the next tick.
        public Ps3Reply SendCommand(string command, byte[] upload = null)
        {
            lock (sendLock)
            {
                NetworkStream s = stream;
                if (s == null) return Ps3Reply.Error("not connected");
                try
                {
                    replyReady.Reset();
                    pendingReply = null;

                    byte[] cmd = Encoding.ASCII.GetBytes(command + "\n");
                    s.Write(cmd, 0, cmd.Length);
                    if (upload != null && upload.Length > 0)
                        s.Write(upload, 0, upload.Length);

                    if (!replyReady.WaitOne(60000, false))
                    {
                        DropSocket();
                        return Ps3Reply.Error("reply timeout");
                    }
                    Ps3Reply r = pendingReply ?? Ps3Reply.Error("no reply");
                    pendingReply = null;
                    return r;
                }
                catch (Exception ex)
                {
                    DropSocket();
                    return Ps3Reply.Error(ex.Message);
                }
            }
        }

        // reader thread: pulls framed messages off the socket and dispatches
        // them. dies when the socket drops; AutoConnectLoop will reconnect.
        private void ReaderLoop()
        {
            try
            {
                while (running)
                {
                    NetworkStream s = stream;
                    if (s == null) return;

                    string header = ReadHeaderLine(s);
                    int sp = header.IndexOf(' ');
                    if (sp < 0) throw new IOException("malformed header: " + header);
                    string tag = header.Substring(0, sp);
                    int n;
                    if (!int.TryParse(header.Substring(sp + 1), out n) || n < 0)
                        throw new IOException("malformed length: " + header);

                    byte[] payload = new byte[n];
                    int off = 0;
                    while (off < n)
                    {
                        int got = s.Read(payload, off, n - off);
                        if (got <= 0) throw new IOException("closed mid-payload");
                        off += got;
                    }

                    if (tag == "LOG")
                    {
                        var evt = LogReceived;
                        if (evt != null) evt(Encoding.UTF8.GetString(payload));
                    }
                    else
                    {
                        // OK / ERR reply for whichever SendCommand is in flight.
                        lock (replyLock)
                        {
                            pendingReply = new Ps3Reply(tag == "OK", payload);
                            replyReady.Set();
                        }
                    }
                }
            }
            catch
            {
                DropSocket();
            }
        }

        private string ReadHeaderLine(NetworkStream s)
        {
            var sb = new StringBuilder(32);
            for (;;)
            {
                int b = s.ReadByte();
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
            // unblock any waiting SendCommand
            lock (replyLock)
            {
                pendingReply = Ps3Reply.Error("disconnected");
                replyReady.Set();
            }
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
                t.ReceiveTimeout = 0; // reader blocks until LOG frames arrive
                t.SendTimeout    = 60000;
                tcp    = t;
                stream = t.GetStream();

                readerThread = new Thread(ReaderLoop) { IsBackground = true };
                readerThread.Start();
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
