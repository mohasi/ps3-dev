using System.Collections.Generic;
using System.Globalization;
using System.Text.RegularExpressions;

namespace RenpyToPs3.RenPy
{
   // Imagemap menu support. Two faithful forms, both translated into the SAME structure so the player
   // replays them with ONE generic renderer (see _ImageMapper in _layout/imagemap_common.rpym):
   //
   //  1. Simple, returns-a-value:
   //       result = renpy.imagemap(ground, hover, [(x0,y0,x1,y1,"value"), ...])
   //     A full-screen `ground` with hotspot rects that show `hover` clipped to them and return their
   //     value on click. (Old RE:Alistair main menu, etc.) -> Kind = "" (empty).
   //
   //  2. Themed, the layout imagemap screens (imagemap_common's _ImageMapper):
   //       layout.imagemap_<screen>(ground, idle, hover, selected_idle, selected_hover, hotspots)
   //     used uniformly by navigation / load_save / preferences (5 images) and yesno (3) / main_menu (2).
   //     Each hotspot has a NAME (not a value): a nav label ("Return"/"Save Game"/...), a file-picker
   //     slot ("slot_0".."slot_N"), a page ("page_1".."page_M","page_auto","page_quick"),
   //     "previous"/"next", or a preferences toggle/bar name. The player dispatches by name exactly as
   //     the engine does -> Kind = "navigation"/"load_save"/"preferences"/"yesno_prompt"/"main_menu".
   //
   // We bake every state image + every named hotspot; the player carries the generic engine algorithm
   // (slot/page enumeration, paging, file listing, action dispatch). Hotspots with non-literal coords are
   // skipped (the rest still work).

   public sealed class ImageMapHotspot
   {
      public int X0, Y0, X1, Y1;   // corner rect in native pixels
      public string Name;          // return value (simple) OR hotspot name (themed)
   }

   public sealed class ImageMapDef
   {
      public string Kind = "";              // "" simple; else themed screen name
      public string Ground = "";
      public string Idle = "";              // themed: idle state (falls back to ground)
      public string Hover = "";             // simple: the hover image; themed: hover state
      public string SelectedIdle = "";      // themed
      public string SelectedHover = "";     // themed
      public readonly List<ImageMapHotspot> Hotspots = new List<ImageMapHotspot>();

      public string Key()
      {
         System.Text.StringBuilder sb = new System.Text.StringBuilder();
         sb.Append(Kind).Append('|').Append(Ground).Append('|').Append(Idle).Append('|')
           .Append(Hover).Append('|').Append(SelectedIdle).Append('|').Append(SelectedHover).Append('|');
         foreach (ImageMapHotspot h in Hotspots)
            sb.Append(h.X0).Append(',').Append(h.Y0).Append(',').Append(h.X1).Append(',').Append(h.Y1).Append('=').Append(h.Name).Append(';');
         return sb.ToString();
      }
   }

   public static class ImageMapCompiler
   {
      // Simple form: renpy.imagemap(ground, hover, ...)
      private static readonly Regex GroundHover =
         new Regex(@"renpy\.imagemap\(\s*[uU]?[""']([^""']+)[""']\s*,\s*[uU]?[""']([^""']+)[""']", RegexOptions.Singleline);

      // Themed form: layout.imagemap_<screen>( ... )  -- capture screen name and the arg span.
      private static readonly Regex Themed =
         new Regex(@"layout\.imagemap_(main_menu|navigation|preferences|yesno_prompt|load_save)\s*\(", RegexOptions.Singleline);

      // A (x0,y0,x1,y1,"name") tuple inside a hotspots list.
      private static readonly Regex Hotspot =
         new Regex(@"\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*[uU]?[""']([^""']+)[""']\s*\)", RegexOptions.Singleline);

      // A leading string literal argument (image path), e.g.  "GUI/foo.png"  or  u'bar' .
      private static readonly Regex StrArg =
         new Regex(@"[uU]?[""']([^""']*)[""']", RegexOptions.Singleline);

      // Parse the simple renpy.imagemap(...) form. Returns null if not present / no literal hotspots.
      public static ImageMapDef Parse(string src)
      {
         if (string.IsNullOrEmpty(src)) return null;
         Match gh = GroundHover.Match(src);
         if (!gh.Success) return null;

         ImageMapDef d = new ImageMapDef();
         d.Kind = "";
         d.Ground = gh.Groups[1].Value;
         d.Hover = gh.Groups[2].Value;
         AddHotspots(d, src.Substring(gh.Index));
         return d.Hotspots.Count > 0 ? d : null;
      }

      // Parse every themed layout.imagemap_<screen>(...) call found in src. Returns each as its own def.
      public static List<ImageMapDef> ParseThemed(string src)
      {
         List<ImageMapDef> result = new List<ImageMapDef>();
         if (string.IsNullOrEmpty(src)) return result;

         foreach (Match m in Themed.Matches(src))
         {
            string screen = m.Groups[1].Value;
            int open = m.Index + m.Length - 1;          // index of '('
            int span = MatchParens(src, open);
            if (span < 0) continue;
            string args = src.Substring(open + 1, span - open - 1);

            // Split images (leading string args, before the hotspots list) from the hotspots list.
            int br = args.IndexOf('[');
            string head = br >= 0 ? args.Substring(0, br) : args;

            List<string> imgs = new List<string>();
            foreach (Match s in StrArg.Matches(head))
               imgs.Add(s.Groups[1].Value);

            ImageMapDef d = new ImageMapDef();
            d.Kind = screen;
            AssignImages(d, screen, imgs);
            if (br >= 0) AddHotspots(d, args.Substring(br));
            if (d.Hotspots.Count > 0) result.Add(d);
         }
         return result;
      }

      // Map the leading image args onto state roles per each convenience function's signature
      // (00layout.rpy). Fallbacks mirror _ImageMapper.__init__: idle->ground, hover->idle,
      // selected_idle->idle, selected_hover->hover.
      private static void AssignImages(ImageMapDef d, string screen, List<string> imgs)
      {
         string ground = imgs.Count > 0 ? imgs[0] : "";
         d.Ground = ground;

         if (screen == "main_menu")
         {
            // imagemap_main_menu(ground, selected, hotspots, idle=None)
            d.Idle = ground;
            d.Hover = ground;
            d.SelectedIdle = imgs.Count > 1 ? imgs[1] : ground;
            d.SelectedHover = d.SelectedIdle;
         }
         else if (screen == "yesno_prompt")
         {
            // imagemap_yesno_prompt(ground, idle, hover, hotspots)
            d.Idle = imgs.Count > 1 ? imgs[1] : ground;
            d.Hover = imgs.Count > 2 ? imgs[2] : d.Idle;
            d.SelectedIdle = d.Idle;
            d.SelectedHover = d.Hover;
         }
         else
         {
            // navigation / preferences / load_save:
            //   (ground, idle, hover, selected_idle, selected_hover, hotspots)
            d.Idle = imgs.Count > 1 ? imgs[1] : ground;
            d.Hover = imgs.Count > 2 ? imgs[2] : d.Idle;
            d.SelectedIdle = imgs.Count > 3 ? imgs[3] : d.Idle;
            d.SelectedHover = imgs.Count > 4 ? imgs[4] : d.Hover;
         }
      }

      private static void AddHotspots(ImageMapDef d, string src)
      {
         foreach (Match m in Hotspot.Matches(src))
         {
            ImageMapHotspot h = new ImageMapHotspot();
            h.X0 = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
            h.Y0 = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
            h.X1 = int.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture);
            h.Y1 = int.Parse(m.Groups[4].Value, CultureInfo.InvariantCulture);
            h.Name = m.Groups[5].Value;
            d.Hotspots.Add(h);
         }
      }

      // Return the index of the ')' matching the '(' at openIndex, or -1.
      private static int MatchParens(string s, int openIndex)
      {
         int depth = 0;
         bool inStr = false;
         char q = '\0';
         for (int i = openIndex; i < s.Length; i++)
         {
            char c = s[i];
            if (inStr)
            {
               if (c == q) inStr = false;
               continue;
            }
            if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
            if (c == '(') depth++;
            else if (c == ')') { depth--; if (depth == 0) return i; }
         }
         return -1;
      }
   }
}
