using System.Text;

namespace DebugBridgeClient
{
   // a single decoded reply from the ps3 bridge.
   // wire format is always "<STATUS> <n>\n[<n bytes>]" where STATUS is OK or ERR.
   public sealed class Ps3Reply
   {
      public bool   Ok      { get; private set; }
      public byte[] Payload { get; private set; }

      public Ps3Reply(bool ok, byte[] payload)
      {
         Ok = ok;
         Payload = payload ?? new byte[0];
      }

      // shorthand for callers that only want the payload as ascii text.
      public string AsText()
      {
         return Encoding.ASCII.GetString(Payload);
      }

      public static Ps3Reply Error(string msg)
      {
         return new Ps3Reply(false, Encoding.ASCII.GetBytes(msg));
      }
   }
}
