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

        // send a command over a fresh tcp connection and return the response
        public string SendCommand(string command)
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
                            return "ERR connection timed out";
                        }
                        tcp.EndConnect(ar);
                        tcp.ReceiveTimeout = 10000;
                        tcp.SendTimeout = 5000;

                        NetworkStream stream = tcp.GetStream();
                        byte[] data = Encoding.ASCII.GetBytes(command + "\n");
                        stream.Write(data, 0, data.Length);

                        StreamReader reader = new StreamReader(stream, Encoding.ASCII);
                        string response = reader.ReadLine();
                        return response ?? "ERR no response";
                    }
                }
                catch
                {
                    return "ERR connection failed";
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
            return SendCommand("ping").StartsWith("OK");
        }
    }
}
