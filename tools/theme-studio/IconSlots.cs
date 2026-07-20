using System.Collections.Generic;

namespace ThemeStudio
{
   // one replaceable xmb icon. sizes come from the p3tcompiler manual: everything is
   // 128x128 except the photo and video defaults, which are wider.
   public class IconSlot
   {
      public readonly string Id;
      public readonly string Label;
      public readonly string Group;
      public readonly int Width;
      public readonly int Height;

      public IconSlot(string group, string id, string label, int width = 128, int height = 128)
      {
         Group = group;
         Id = id;
         Label = label;
         Width = width;
         Height = height;
      }
   }

   // every icon the system software lets a theme replace, in the order the manual lists them.
   public static class IconSlots
   {
      public static readonly IconSlot[] All = {
         new IconSlot("Main row", "icon_user", "Users"),
         new IconSlot("Main row", "icon_setting", "Settings"),
         new IconSlot("Main row", "icon_photo", "Photo"),
         new IconSlot("Main row", "icon_music", "Music"),
         new IconSlot("Main row", "icon_video", "Video"),
         new IconSlot("Main row", "icon_game", "Game"),
         new IconSlot("Main row", "icon_network", "Network"),
         new IconSlot("Main row", "icon_friend", "Friends"),
         new IconSlot("Main row", "icon_tv", "TV"),

         new IconSlot("Users", "icon_poweroff", "Turn Off System"),
         new IconSlot("Users", "icon_newuser", "Create New User"),

         new IconSlot("Settings", "icon_update", "System Update"),
         new IconSlot("Settings", "icon_game_setting", "Game Settings"),
         new IconSlot("Settings", "icon_bdvd_setting", "Video Settings"),
         new IconSlot("Settings", "icon_music_setting", "Music Settings"),
         new IconSlot("Settings", "icon_chat_setting", "Chat Settings"),
         new IconSlot("Settings", "icon_system_setting", "System Settings"),
         new IconSlot("Settings", "icon_theme_setting", "Theme Settings"),
         new IconSlot("Settings", "icon_datetime_setting", "Date and Time Settings"),
         new IconSlot("Settings", "icon_powersave_setting", "Power Save Settings"),
         new IconSlot("Settings", "icon_accessory", "Accessory Settings"),
         new IconSlot("Settings", "icon_printer_setting", "Printer Settings"),
         new IconSlot("Settings", "icon_display_setting", "Display Settings"),
         new IconSlot("Settings", "icon_sound_setting", "Sound Settings"),
         new IconSlot("Settings", "icon_security_setting", "Security Settings"),
         new IconSlot("Settings", "icon_remoteplay_setting", "Remote Play Settings"),
         new IconSlot("Settings", "icon_network_setting", "Network Settings"),
         new IconSlot("Settings", "icon_setting_item", "Settings submenu item"),

         new IconSlot("Media", "icon_photo_default", "Photo (default)", 170, 128),
         new IconSlot("Media", "icon_photo_album_default", "Photo album (default)", 170, 128),
         new IconSlot("Media", "icon_music_default", "Music (default)"),
         new IconSlot("Media", "icon_music_album_default", "Music album (default)"),
         new IconSlot("Media", "icon_video_default", "Video (default)", 228, 128),
         new IconSlot("Media", "icon_video_album_default", "Video album (default)", 228, 128),
         new IconSlot("Media", "icon_playing", "Now playing"),
         new IconSlot("Media", "icon_mediaserver_search", "Search for Media Servers"),
         new IconSlot("Media", "icon_playlist", "Playlists"),
         new IconSlot("Media", "icon_playlist_add", "Create New Playlist"),
         new IconSlot("Media", "icon_video_upload", "Video Editor & Uploader"),

         new IconSlot("Devices", "icon_ms", "Memory Stick"),
         new IconSlot("Devices", "icon_cf", "Compact Flash"),
         new IconSlot("Devices", "icon_sd", "SD Memory Card"),
         new IconSlot("Devices", "icon_usb", "USB Device"),
         new IconSlot("Devices", "icon_psp", "PSP"),
         new IconSlot("Devices", "icon_pspms", "PSP Memory Stick"),
         new IconSlot("Devices", "icon_usbcamera", "Digital Camera"),
         new IconSlot("Devices", "icon_usbaad", "ATRAC Audio Device"),

         new IconSlot("Game", "icon_gamedata", "Game Data Utility"),
         new IconSlot("Game", "icon_vmc", "Memory Card Utility (PS/PS2)"),
         new IconSlot("Game", "icon_savedata", "Save Data Utility"),
         new IconSlot("Game", "icon_savedata_minis", "Save Data Utility (minis)"),
         new IconSlot("Game", "icon_newvmc", "New Internal Memory Card"),
         new IconSlot("Game", "icon_trophy", "Trophy Collection"),

         new IconSlot("Network", "icon_onlinemanual", "Online Instruction Manuals"),
         new IconSlot("Network", "icon_remoteplay", "Remote Play"),
         new IconSlot("Network", "icon_inet_search", "Internet Search"),
         new IconSlot("Network", "icon_browser", "Internet Browser"),
         new IconSlot("Network", "icon_download", "Download Management"),
         new IconSlot("Network", "icon_accountmanage", "Account Management"),

         new IconSlot("Friends", "icon_blocklist", "Block List"),
         new IconSlot("Friends", "icon_addfriend", "Add a Friend"),
         new IconSlot("Friends", "icon_playermet", "Players Met"),
         new IconSlot("Friends", "icon_chat", "Start New Chat"),
         new IconSlot("Friends", "icon_chatroom", "Chat Room"),
         new IconSlot("Friends", "icon_chatroom_text", "Chat Room (Text Only)"),
         new IconSlot("Friends", "icon_mbox", "Message Box"),
         new IconSlot("Friends", "icon_mbox_received", "Received"),
         new IconSlot("Friends", "icon_mbox_sent", "Sent"),
         new IconSlot("Friends", "icon_mbox_create", "Create Message"),

         new IconSlot("Fallback", "icon_default_h", "Any unset row icon"),
         new IconSlot("Fallback", "icon_default_v", "Any unset column icon")
      };

      public static IEnumerable<string> Groups
      {
         get
         {
            var seen = new List<string>();
            foreach (IconSlot slot in All)
               if (!seen.Contains(slot.Group)) { seen.Add(slot.Group); yield return slot.Group; }
         }
      }
   }
}
