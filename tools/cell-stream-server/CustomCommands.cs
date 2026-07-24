using System;
using System.Diagnostics;
using Microsoft.Win32;

namespace CellStreamServer
{
   // one of the four PC actions the PS3 can fire by slot number. Run starts a program or URI;
   // Guide taps the controller's Guide button (which is how Windows opens Game Bar).
   internal enum CustomCommandKind { None, Run, Guide }

   internal sealed class CustomCommand
   {
      public CustomCommandKind Kind = CustomCommandKind.None;
      public string Value = "";   // Run: a command line or URI (e.g. steam://open/bigpicture); Guide: unused
      public string Label = "";   // short name shown in the log (and the PS3 toast, later)
   }

   // the four slots the PS3 triggers with CUSTOM 1..4. the PS3 only sends the slot number; what each
   // slot does is defined here and on the Custom Commands tab, and remembered in the registry. a device
   // on the LAN can only ask for a slot, never send a raw command, so it can't make the PC run anything
   // the user did not set up here.
   internal static class CustomCommands
   {
      public const int SlotCount = 4;
      private const string SettingsKey = @"Software\CellStreamServer";

      private static readonly CustomCommand[] slots = LoadAll();

      public static CustomCommand Get(int slot)   // slot is 1..SlotCount
      {
         return slot >= 1 && slot <= SlotCount ? slots[slot - 1] : null;
      }

      public static void Set(int slot, CustomCommand command)
      {
         if (slot < 1 || slot > SlotCount) return;
         slots[slot - 1] = command;
         Save(slot, command);
      }

      // runs the action bound to a slot. a Guide tap needs the virtual gamepad, so it goes through the
      // pad receiver that owns it.
      public static void Run(int slot, PadReceiver padReceiver)
      {
         CustomCommand command = Get(slot);
         if (command == null || command.Kind == CustomCommandKind.None)
         {
            Server.Log("custom " + slot + ": nothing is bound to this slot");
            return;
         }

         switch (command.Kind)
         {
            case CustomCommandKind.Run:
               try
               {
                  Process.Start(new ProcessStartInfo(command.Value) { UseShellExecute = true });
                  Server.Log("custom " + slot + ": ran " + command.Value);
               }
               catch (Exception exception)
               {
                  Server.Log("custom " + slot + ": could not run " + command.Value + " - " + exception.Message);
               }
               break;

            case CustomCommandKind.Guide:
               if (padReceiver.PressGamepadGuide())
                  Server.Log("custom " + slot + ": tapped Guide (Game Bar)");
               else
                  Server.Log("custom " + slot + ": no virtual gamepad - install ViGEmBus for Game Bar (use it once in controller mode)");
               break;
         }
      }

      // first run seeds slot 1 = Game Bar and slot 2 = Steam Big Picture, matching the PS3's defaults
      private static CustomCommand[] LoadAll()
      {
         var loaded = new CustomCommand[SlotCount];
         bool anySaved = false;
         try
         {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(SettingsKey))
            {
               for (int slot = 1; slot <= SlotCount; slot++)
               {
                  string kind = key == null ? null : key.GetValue("Custom" + slot + "Kind") as string;
                  if (kind == null) { loaded[slot - 1] = new CustomCommand(); continue; }
                  anySaved = true;
                  loaded[slot - 1] = new CustomCommand
                  {
                     Kind = ParseKind(kind),
                     Value = (key.GetValue("Custom" + slot + "Value") as string) ?? "",
                     Label = (key.GetValue("Custom" + slot + "Label") as string) ?? ""
                  };
               }
            }
         }
         catch { }
         for (int i = 0; i < SlotCount; i++) if (loaded[i] == null) loaded[i] = new CustomCommand();

         if (!anySaved) SeedDefaults(loaded);
         return loaded;
      }

      private static void SeedDefaults(CustomCommand[] into)
      {
         into[0] = new CustomCommand { Kind = CustomCommandKind.Guide, Label = "Game Bar" };
         into[1] = new CustomCommand { Kind = CustomCommandKind.Run, Value = "steam://open/bigpicture", Label = "Big Picture" };
         for (int slot = 1; slot <= SlotCount; slot++) Save(slot, into[slot - 1]);
      }

      private static void Save(int slot, CustomCommand command)
      {
         try
         {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(SettingsKey))
            {
               if (key == null) return;
               key.SetValue("Custom" + slot + "Kind", command.Kind.ToString());
               key.SetValue("Custom" + slot + "Value", command.Value ?? "");
               key.SetValue("Custom" + slot + "Label", command.Label ?? "");
            }
         }
         catch (Exception exception)
         {
            Server.Log("custom " + slot + ": could not save - " + exception.Message);
         }
      }

      private static CustomCommandKind ParseKind(string text)
      {
         try { return (CustomCommandKind)Enum.Parse(typeof(CustomCommandKind), text); }
         catch { return CustomCommandKind.None; }
      }
   }
}
