namespace ThemeStudio
{
   // the mouse cursors a theme can replace. base_x/base_y are the point that actually does the
   // clicking, measured from the top-left of the 48x48 picture.
   public class PointerSlot
   {
      public readonly string Id;
      public readonly string Label;
      public readonly int ClickX;
      public readonly int ClickY;

      public PointerSlot(string id, string label, int clickX, int clickY)
      {
         Id = id;
         Label = label;
         ClickX = clickX;
         ClickY = clickY;
      }
   }

   // the five sound effects a theme can replace. each is a mono or stereo .vag.
   public class SoundSlot
   {
      public readonly string Id;
      public readonly string Label;

      public SoundSlot(string id, string label)
      {
         Id = id;
         Label = label;
      }
   }

   public static class PointerSlots
   {
      public const int Size = 48;

      // the defaults are sony's own click points from the sample theme
      public static readonly PointerSlot[] All = {
         new PointerSlot("pointer_arrow", "Normal arrow", 20, 2),
         new PointerSlot("pointer_finger", "Over something clickable", 16, 10),
         new PointerSlot("pointer_click", "While clicking", 16, 10),
         new PointerSlot("pointer_hand", "Over something draggable", 23, 17),
         new PointerSlot("pointer_grab", "While dragging", 23, 17),
         new PointerSlot("pointer_pen", "Over a text box", 6, 38)
      };
   }

   public static class SoundSlots
   {
      public static readonly SoundSlot[] All = {
         new SoundSlot("se_cursor", "Moving the cursor"),
         new SoundSlot("se_decide", "Confirming"),
         new SoundSlot("se_cancel", "Going back"),
         new SoundSlot("se_optionmenu", "Opening the options menu"),
         new SoundSlot("se_system_ok", "A message appearing")
      };
   }
}
