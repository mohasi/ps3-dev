using System;
using System.Configuration;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Threading;

namespace DebugBridgeClient
{
    // manages the tcp connection to the ps3 simple-debug-bridge plugin (port 8785)
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

        public string Host { get { return host; } }

        public bool IsConnected
        {
            get { return connected; }
        }

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
        }

        // send a command over a fresh tcp connection. reads all response lines
        // until an OK/ERR terminator. returns body lines + terminator. on
        // transport failure returns a single "ERR ..." element.
        public string[] SendCommand(string command)
        {
            return SendCommandWithPayload(command, null);
        }

        // same, but after the command line writes `payload` raw bytes (used
        // for binary-upload commands like vsh-plugin-install).
        public string[] SendCommandWithPayload(string command, byte[] payload)
        {
            lock (sendLock)
            {
                try
                {
                    using (TcpClient tcp = new TcpClient())
                    {
                        IAsyncResult ar = tcp.BeginConnect(host, Port, null, null);
                        if (!ar.AsyncWaitHandle.WaitOne(3000, false))
                        {
                            tcp.Close();
                            return new[] { "ERR connection timed out" };
                        }
                        tcp.EndConnect(ar);
                        // payload uploads need plenty of time; pick generously.
                        tcp.ReceiveTimeout = payload != null ? 60000 : 10000;
                        tcp.SendTimeout    = payload != null ? 60000 : 5000;

                        NetworkStream stream = tcp.GetStream();
                        byte[] data = Encoding.ASCII.GetBytes(command + "\n");
                        stream.Write(data, 0, data.Length);
                        if (payload != null && payload.Length > 0)
                        {
                            stream.Write(payload, 0, payload.Length);
                        }

                        StreamReader reader = new StreamReader(stream, Encoding.ASCII);
                        var lines = new System.Collections.Generic.List<string>();
                        string line;
                        while ((line = reader.ReadLine()) != null)
                        {
                            lines.Add(line);
                            if (line.StartsWith("OK") || line.StartsWith("ERR")) break;
                        }
                        if (lines.Count == 0) lines.Add("ERR no response");
                        return lines.ToArray();
                    }
                }
                catch
                {
                    return new[] { "ERR connection failed" };
                }
            }
        }

        private void AutoConnectLoop()
        {
            while (running)
            {
                bool reachable = Probe();
                if (reachable != connected)
                {
                    connected = reachable;
                    EventHandler evt = reachable ? Connected : Disconnected;
                    if (evt != null) evt(this, EventArgs.Empty);
                }
                Thread.Sleep(ReconnectDelayMs);
            }
        }

        private bool Probe()
        {
            string[] lines = SendCommand("ping");
            return lines.Length > 0 && lines[lines.Length - 1].StartsWith("OK");
        }

        // result of a binary download. on success Status starts with "OK <bytes>",
        // Data is the raw payload. on failure Status is "ERR ...", Data is null.
        public class DownloadResult
        {
            public string Status;
            public byte[] Data;
        }

        // download a file (or window of one) from the ps3.
        // protocol: send "<command>\n", read one header line, then exactly N
        // bytes of payload as advertised in the header.
        public DownloadResult Download(string command)
        {
            lock (sendLock)
            {
                var result = new DownloadResult();
                try
                {
                    using (TcpClient tcp = new TcpClient())
                    {
                        IAsyncResult ar = tcp.BeginConnect(host, Port, null, null);
                        if (!ar.AsyncWaitHandle.WaitOne(3000, false))
                        {
                            tcp.Close();
                            result.Status = "ERR connection timed out";
                            return result;
                        }
                        tcp.EndConnect(ar);
                        tcp.ReceiveTimeout = 60000;
                        tcp.SendTimeout    = 5000;

                        NetworkStream stream = tcp.GetStream();
                        byte[] cmd = Encoding.ASCII.GetBytes(command + "\n");
                        stream.Write(cmd, 0, cmd.Length);

                        // read header line up to '\n' (max 128 bytes is plenty).
                        var hdr = new StringBuilder();
                        for (int i = 0; i < 128; i++)
                        {
                            int b = stream.ReadByte();
                            if (b < 0) break;
                            if (b == '\n') break;
                            if (b != '\r') hdr.Append((char)b);
                        }
                        string header = hdr.ToString();
                        result.Status = header;

                        if (!header.StartsWith("OK ")) return result;

                        int sp = header.IndexOf(' ', 3);
                        string sizeStr = sp < 0 ? header.Substring(3) : header.Substring(3, sp - 3);
                        int size;
                        if (!int.TryParse(sizeStr, out size) || size < 0)
                        {
                            result.Status = "ERR bad header: " + header;
                            return result;
                        }

                        var data = new byte[size];
                        int off = 0;
                        while (off < size)
                        {
                            int n = stream.Read(data, off, size - off);
                            if (n <= 0) { result.Status = "ERR short read"; return result; }
                            off += n;
                        }
                        result.Data = data;
                        return result;
                    }
                }
                catch (Exception ex)
                {
                    result.Status = "ERR " + ex.Message;
                    return result;
                }
            }
        }
    }
}
