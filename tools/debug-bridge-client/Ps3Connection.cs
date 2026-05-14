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
                        tcp.ReceiveTimeout = 10000;
                        tcp.SendTimeout = 5000;

                        NetworkStream stream = tcp.GetStream();
                        byte[] data = Encoding.ASCII.GetBytes(command + "\n");
                        stream.Write(data, 0, data.Length);

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
    }
}
