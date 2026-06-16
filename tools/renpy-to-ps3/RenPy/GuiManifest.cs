using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace RenpyToPs3.RenPy
{
    // Extracts a normalized, game-agnostic GUI descriptor from a compiled program. Ren'Py has
    // two GUI dialects: the old per-`style` system (6.x: style.window/style.default/...) and the
    // newer `gui.*` variable system (7.x/8.x). We read whichever is present and emit one flat
    // key=value manifest (the "game.gui" rpk entry) that the player renders generically -- so
    // there is zero per-game code on the console. Everything is optional; the player supplies
    // sane defaults for any missing key.
    //
    // Emitted keys (values in the game's NATIVE pixel space; the player scales to its letterbox):
    //   native_w, native_h          game's design resolution (text/box metrics are relative to it)
    //   text_font                   dialogue font (basename)
    //   text_size                   dialogue text size
    //   text_color                  #rrggbb
    //   text_shadow                 dx,dy   text_shadow_color #rrggbb
    //   name_size, name_color       speaker-label defaults
    //   name_spacing, name_bold     say_vbox.spacing / say_label.bold
    //   textbox_bg                  textbox background image (basename); 9-slice if framed
    //   textbox_frame               l,t,r,b  (9-slice insets; absent => stretch whole image)
    //   textbox_color               textbox SOLID background #rrggbb[aa] (when no image is set)
    //   textbox_height              textbox height in native px (when known, e.g. gui.textbox_height)
    //   textbox_xmargin, textbox_margin_t/_b, textbox_pad_x/_y, textbox_ymin
    //                               the style.window box model (margins/padding/yminimum)
    //   choice_bg, choice_hover_bg  in-game menu button backgrounds
    //   choice_color, choice_hover_color
    //   ctc, ctc_xpos/_ypos/_xanchor/_yanchor, ctc_fixed   click-to-continue icon
    //   nvl_pad_x/_y, nvl_spacing, nvl_bg                  NVL-mode page styling
    //   char_color.<DisplayName>    per-speaker name colour from Character(color=...)
    public static class GuiManifest
    {
        public static string Build(IrProgram p)
        {
            // Gather every source line that might carry a setting (style/gui/config/Character).
            List<string> lines = new List<string>();
            foreach (string s in p.Strings)
                // Gather any line that might carry a setting. Use a broad `config.` match (not a
                // per-key allow-list) so no config setting is silently dropped -- e.g.
                // config.default_text_cps lived in a string with no other trigger and was being
                // missed, which is why the typewriter never ran.
                if (s.IndexOf("style.", System.StringComparison.Ordinal) >= 0 ||
                    s.IndexOf("gui.", System.StringComparison.Ordinal) >= 0 ||
                    s.IndexOf("config.", System.StringComparison.Ordinal) >= 0 ||
                    s.IndexOf("theme.", System.StringComparison.Ordinal) >= 0 ||
                    s.IndexOf("Character(", System.StringComparison.Ordinal) >= 0)
                    foreach (string ln in s.Replace("\r\n", "\n").Split('\n')) lines.Add(ln.Trim());

            // last-wins: later assignments (e.g. in init blocks) override earlier defaults.
            Dictionary<string, string> f = new Dictionary<string, string>(System.StringComparer.Ordinal);

            // Game title (config.window_title) -> `title`, shown by the player's game picker instead of
            // the .rpk filename. e.g. config.window_title = u"RE: Alistair++".
            foreach (string ln in lines)
            {
                Match tm = Regex.Match(ln, @"config\.window_title\s*=\s*u?[""']([^""']*)[""']");
                if (tm.Success) f["title"] = tm.Groups[1].Value.Trim();
                // config.main_menu_music -> mm_music (the player plays it while the main menu shows).
                Match mm = Regex.Match(ln, @"config\.main_menu_music\s*=\s*u?[""']([^""']*)[""']");
                if (mm.Success) f["mm_music"] = mm.Groups[1].Value.Trim();
            }

            // Seed the engine's CLASSIC (6.x) default textbox box-model so we translate exact
            // values rather than guess. These are Ren'Py's own defaults from
            // renpy/common/_compat/styles.rpym (style.window): a game inherits them and overrides
            // only what it sets. Modern gui.* games use a different model, so only seed for classic.
            bool modern = HasGui(lines);
            if (!modern)
            {
                f["textbox_xmargin"] = "10";   // style.window.xmargin
                f["textbox_margin_t"] = "5";   // style.window.ymargin (top)
                f["textbox_margin_b"] = "5";   // style.window.ymargin (bottom)
                f["textbox_pad_x"] = "10";     // style.window.xpadding
                f["textbox_pad_y"] = "5";      // style.window.ypadding
                f["textbox_ymin"] = "150";     // style.window.yminimum (incl. margins + padding)
                f["textbox_color"] = "#00008080"; // style.window.background = Solid((0,0,128,128)) (_compat/styles.rpym)
                f["name_spacing"] = "8";       // style.say_vbox.spacing (name -> dialogue gap, 00style.rpy)
                f["name_bold"] = "1";          // style.say_label.bold = True (00style.rpy)

                // NVL defaults (renpy/common/00nvl_mode.rpy): nvl_window bg #0008 (-> #000088),
                // xpadding=20, ypadding=30; nvl_vbox box_spacing=10.
                f["nvl_pad_x"] = "20";
                f["nvl_pad_y"] = "30";
                f["nvl_spacing"] = "10";
                f["nvl_bg"] = "#0008";   // Ren'Py RGBA shorthand: black @ alpha 0x88

                // Classic themes (00themes.rpy `_theme`, e.g. theme.roundrect) override the
                // window box-model when given a window colour: xpadding/ypadding=6 and
                // xmargin/ymargin = 6 if rounded_window else 0. Apply faithfully so themed games
                // (flush-left, tighter padding) match. The game's explicit style.window.* lines
                // below still win (last-wins).
                bool usesTheme = false, rounded = true, roundedSeen = false;
                foreach (string ln in lines)
                {
                    if (Regex.IsMatch(ln, @"\btheme\.\w+\s*\(")) usesTheme = true;
                    Match rm = Regex.Match(ln, @"rounded_window\s*=\s*(True|False)");
                    if (rm.Success) { rounded = rm.Groups[1].Value == "True"; roundedSeen = true; }
                }
                if (usesTheme)
                {
                    f["textbox_pad_x"] = "6";
                    f["textbox_pad_y"] = "6";
                    string m = (roundedSeen ? rounded : true) ? "6" : "0";
                    f["textbox_xmargin"] = m;
                    f["textbox_margin_t"] = m;
                    f["textbox_margin_b"] = m;
                    // A theme replaces style.window.background with an engine-GENERATED image
                    // (im.* rounded boxes) we cannot extract -- capability gap. Drop the classic
                    // solid seed; the game's own Frame()/background override below still wins.
                    f.Remove("textbox_color");
                    p.Notes.Add("theme window background is engine-generated (not extractable); " +
                                "textbox shows the game's own background override if present");
                }
            }

            foreach (string ln in lines)
            {
                // ---- native resolution ----
                Grab(ln, @"config\.screen_width\s*=\s*(\d+)", f, "native_w", FmtRaw);
                Grab(ln, @"config\.screen_height\s*=\s*(\d+)", f, "native_h", FmtRaw);

                // ---- save-thumbnail size (config.thumbnail_width/height) ----
                // The engine takes the slot screenshot at this size (renpy.take_screenshot) and the file
                // picker draws the screenshot Displayable at exactly this size. Last assignment wins,
                // matching the engine (the game's own init runs after the common-layout defaults).
                Grab(ln, @"config\.thumbnail_width\s*=\s*(\d+)", f, "thumb_w", FmtRaw);
                Grab(ln, @"config\.thumbnail_height\s*=\s*(\d+)", f, "thumb_h", FmtRaw);

                // ---- typewriter speed (config.default_text_cps; 0/absent = instant) ----
                Grab(ln, @"config\.default_text_cps\s*=\s*(\d+)", f, "text_cps", FmtRaw);
                MatchTwo(ln, @"gui\.init\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)", f, "native_w", "native_h");

                // ---- text metric era ----
                // Ren'Py 6.12 rewrote the text system (ftfont: fractional advances, no
                // kerning). Older games went through SDL_ttf, which rounds every glyph
                // advance UP to whole pixels (FT_CEIL) and applies kerning -- line breaks
                // land differently. config.script_version pins the era; the player
                // emulates the old metrics when text_advance=ceil.
                MatchScriptVersion(ln, f);

                // ---- dialogue text (6.x style.default / style.say_dialogue) ----
                Grab(ln, @"style\.(?:default|say_dialogue)\.font\s*=\s*(.+)", f, "text_font", FmtName);
                Grab(ln, @"style\.(?:default|say_dialogue)\.size\s*=\s*(\d+)", f, "text_size", FmtRaw);
                Grab(ln, @"style\.(?:default|say_dialogue)\.color\s*=\s*(.+)", f, "text_color", FmtColor);
                MatchTwoJoin(ln, @"style\.(?:default|say_dialogue)\.drop_shadow\s*=\s*\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)", f, "text_shadow");
                Grab(ln, @"style\.(?:default|say_dialogue)\.drop_shadow_color\s*=\s*(.+)", f, "text_shadow_color", FmtColor);

                // ---- speaker label (6.x style.say_label) ----
                Grab(ln, @"style\.say_label\.size\s*=\s*(\d+)", f, "name_size", FmtRaw);
                Grab(ln, @"style\.say_label\.color\s*=\s*(.+)", f, "name_color", FmtColor);

                // ---- textbox background + margins (6.x style.window / style.say_window) ----
                MatchFrame(ln, f);
                MatchWindowColor(ln, f);
                Grab(ln, @"style\.(?:say_)?window\.background\s*=\s*[""']([^""'#][^""']*)[""']\s*$", f, "textbox_bg", FmtName);
                Grab(ln, @"style\.(?:say_)?window\.bottom_margin\s*=\s*(\d+)", f, "textbox_margin_b", FmtRaw);
                Grab(ln, @"style\.(?:say_)?window\.top_margin\s*=\s*(\d+)", f, "textbox_margin_t", FmtRaw);
                MatchYmargin(ln, f);
                Grab(ln, @"style\.(?:say_)?window\.xpadding\s*=\s*(\d+)", f, "textbox_pad_x", FmtRaw);
                Grab(ln, @"style\.(?:say_)?window\.ypadding\s*=\s*(\d+)", f, "textbox_pad_y", FmtRaw);
                // Asymmetric per-edge paddings (style.window.{left,right,top,bottom}_padding). Many games
                // set these instead of xpadding/ypadding (e.g. Alistair 180/210/95/30); the player uses
                // them per-edge and falls back to pad_x/pad_y when absent. Also the in-game chat inherits
                // top_padding. Universal style properties -- resolved here, no engine at runtime.
                Grab(ln, @"style\.(?:say_)?window\.left_padding\s*=\s*(\d+)",   f, "textbox_pad_l", FmtRaw);
                Grab(ln, @"style\.(?:say_)?window\.right_padding\s*=\s*(\d+)",  f, "textbox_pad_r", FmtRaw);
                Grab(ln, @"style\.(?:say_)?window\.top_padding\s*=\s*(\d+)",    f, "textbox_pad_t", FmtRaw);
                Grab(ln, @"style\.(?:say_)?window\.bottom_padding\s*=\s*(\d+)", f, "textbox_pad_b", FmtRaw);
                Grab(ln, @"style\.(?:say_)?window\.xmargin\s*=\s*(\d+)", f, "textbox_xmargin", FmtRaw);
                Grab(ln, @"style\.(?:say_)?window\.yminimum\s*=\s*(\d+)", f, "textbox_ymin", FmtRaw);

                // ---- click-to-continue icon (Character(ctc=anim.Blink("arrow.png", xpos=.., ypos=..))) ----
                Grab(ln, @"ctc\s*=\s*anim\.\w+\(\s*[""']([^""']+)[""']", f, "ctc", FmtName);
                Grab(ln, @"gui\.ctc\s*=\s*[""']([^""']+)[""']", f, "ctc", FmtName);
                // "fixed" ctc draws the icon at the xpos/ypos given in its own definition
                // (Character ctc_position default is "nestled" = inline; renpy defaultstore.py).
                Grab(ln, @"ctc=\s*anim\.\w+\([^)]*\bxpos\s*=\s*(\d+)", f, "ctc_xpos", FmtRaw);
                Grab(ln, @"ctc=\s*anim\.\w+\([^)]*\bypos\s*=\s*(\d+)", f, "ctc_ypos", FmtRaw);
                Grab(ln, @"ctc=\s*anim\.\w+\([^)]*\bxanchor\s*=\s*(\d+)", f, "ctc_xanchor", FmtRaw);
                Grab(ln, @"ctc=\s*anim\.\w+\([^)]*\byanchor\s*=\s*(\d+)", f, "ctc_yanchor", FmtRaw);
                if (Regex.IsMatch(ln, @"ctc_position\s*=\s*[""']fixed[""']")) f["ctc_fixed"] = "1";

                // ---- classic theme main/game menu (theme.roundrect mm_root/gm_root +
                // theme.image_buttons) ----  The player renders the menu from these: mm_bg is
                // the title-screen background; each mm_btn.<Label> = idle|hover image. The
                // button ORDER + actions are engine constants in the player, so this stays
                // game-agnostic (any classic-theme game emits its own bg + button art).
                Grab(ln, @"\bmm_root\s*=\s*[""']([^""']+)[""']", f, "mm_bg", FmtName);
                Grab(ln, @"\bgm_root\s*=\s*[""']([^""']+)[""']", f, "gm_bg", FmtName);
                // theme.roundrect colours (each kwarg on its own line of the gathered call; tuple
                // (r,g,b[,a]) or "#hex"). Absent => engine defaults, supplied by the player.
                //   frame          -> menu-frame bg (style.frame.background = RoundRect(frame))
                //   widget         -> button idle bg     widget_hover -> button hover bg
                //   widget_text    -> button text        widget_selected -> selected button text
                // The buttons use the SAME RoundRect template as the frame, tinted by these. Used by
                // the in-game menu's navigation column (and any classic roundrect text button).
                MatchThemeColor(ln, "frame", f, "mm_frame_color");
                MatchThemeColor(ln, "widget", f, "gm_btn_idle");
                MatchThemeColor(ln, "widget_hover", f, "gm_btn_hover");
                MatchThemeColor(ln, "widget_text", f, "gm_btn_text");
                MatchThemeColor(ln, "widget_selected", f, "gm_btn_selected");
                // disabled / disabled_text -> an insensitive button's RoundRect(disabled) bg +
                // large_button_text.insensitive_color. Drives the empty-slot colour on the Load screen.
                MatchThemeColor(ln, "disabled", f, "gm_btn_disabled");
                MatchThemeColor(ln, "disabled_text", f, "gm_btn_disabled_text");
                Grab(ln, @"^\s*text_size\s*=\s*(\d+)", f, "gm_text_size", FmtRaw);
                // A game can re-parent the menu frame to style.default (or null its background),
                // which drops the inherited RoundRect box + padding -- the buttons then sit on the
                // title image with no frame. (broken_memories does this.)
                if (Regex.IsMatch(ln, @"\bmm_menu_frame\.set_parent\s*\(\s*style\.default\s*\)") ||
                    Regex.IsMatch(ln, @"\bmm_menu_frame\.background\s*=\s*None"))
                    f["mm_frame_none"] = "1";
                // theme.image_buttons: "<label>" : (idle, hover[, selected_idle, selected_hover,
                // insensitive]). Keep idle|hover|selected_idle|insensitive (the states the nav draws:
                // idle, focused-hover, the current-screen "selected" grey, and the disabled grey).
                Match ib = Regex.Match(ln, @"^\s*[""']([^""']+)[""']\s*:\s*\(\s*[""']([^""']+\.png)[""']\s*,\s*[""']([^""']+\.png)[""'](?:\s*,\s*[""']([^""']+\.png)[""'])?(?:\s*,\s*[""']([^""']+\.png)[""'])?(?:\s*,\s*[""']([^""']+\.png)[""'])?");
                if (ib.Success)
                    f["mm_btn." + ib.Groups[1].Value] = ib.Groups[2].Value + "|" + ib.Groups[3].Value
                        + "|" + ib.Groups[4].Value + "|" + ib.Groups[6].Value;   // selected_idle, insensitive ("" if absent)

                // ---- NVL overrides (else the seeded 00nvl_mode.rpy defaults stand) ----
                Grab(ln, @"style\.nvl_window\.background\s*=\s*[""'](#[0-9A-Fa-f]{3,8})[""']", f, "nvl_bg", FmtColorA);
                Grab(ln, @"style\.nvl_window\.xpadding\s*=\s*(\d+)", f, "nvl_pad_x", FmtRaw);
                Grab(ln, @"style\.nvl_window\.ypadding\s*=\s*(\d+)", f, "nvl_pad_y", FmtRaw);
                Grab(ln, @"style\.nvl_vbox\.box_spacing\s*=\s*(\d+)", f, "nvl_spacing", FmtRaw);
                Grab(ln, @"style\.say_vbox\.spacing\s*=\s*(\d+)", f, "name_spacing", FmtRaw);
                if (Regex.IsMatch(ln, @"style\.say_label\.bold\s*=\s*False")) f["name_bold"] = "0";
                else if (Regex.IsMatch(ln, @"style\.say_label\.bold\s*=\s*True")) f["name_bold"] = "1";

                // file picker slot text size (style.file_picker_text.size); engine default is the
                // theme small_text_size (16/12 by res), so only emit when the game overrides it.
                Grab(ln, @"style\.file_picker_text\.size\s*=\s*(\d+)", f, "slot_text_size", FmtRaw);

                // imagemap_load_save slot content placement: each slot is a ui.fixed at the slot hotspot
                // rect, holding the screenshot window (file_picker_ss_window) and the text window
                // (file_picker_text_window) at their xpos/ypos WITHIN the slot. The text colour is
                // style.file_picker_text.color (an override of large_button_text). Extract generically so
                // the player positions slot text/thumbnail per-game from the manifest, never hardcoded.
                Grab(ln, @"style\.file_picker_text_window\.xpos\s*=\s*(\d+)", f, "slot_text_x", FmtRaw);
                Grab(ln, @"style\.file_picker_text_window\.ypos\s*=\s*(\d+)", f, "slot_text_y", FmtRaw);
                Grab(ln, @"style\.file_picker_ss_window\.xpos\s*=\s*(\d+)", f, "slot_ss_x", FmtRaw);
                Grab(ln, @"style\.file_picker_ss_window\.ypos\s*=\s*(\d+)", f, "slot_ss_y", FmtRaw);
                Grab(ln, @"style\.file_picker_text\.color\s*=\s*[""'](#[0-9A-Fa-f]{3,8})[""']", f, "slot_text_color", FmtColorA);

                // ---- in-game choice buttons (6.x) ----
                // background/hover_background are usually a Frame("img", L, T[, R, B]) 9-slice (e.g.
                // Alistair's choicebutton.png 16,16); parse the image + insets like the textbox frame.
                // Fall back to a plain Image string when it isn't a Frame.
                MatchChoiceFrame(ln, "background",       f, "choice_bg",       "choice_frame");
                MatchChoiceFrame(ln, "hover_background", f, "choice_hover_bg", "choice_frame");
                // NOTE: style.menu_choice.idle_color/hover_color in 00style ("#0ff"/"#ff0") are the engine
                // BASE that button_menu()'s set_parent+clear wipes -- NOT the resolved colour. menu_choice =
                // Style(style.default), so its colour resolves to style.default.color (= text_color); the
                // roundrect hover shows via the button background (hover_background), not a text-colour
                // change (theme button_text sets no hover_color). Resolved in the post-pass below.
                // Button box-model + menu position (style.menu_choice_button.* / style.menu_window.yalign).
                Grab(ln, @"style\.menu_choice\.size\s*=\s*(\d+)", f, "choice_size", FmtRaw);
                Grab(ln, @"style\.menu_choice_button\.xminimum\s*=\s*(\d+)", f, "choice_xmin", FmtRaw);
                Grab(ln, @"style\.menu_choice_button\.yminimum\s*=\s*(\d+)", f, "choice_ymin", FmtRaw);
                Grab(ln, @"style\.menu_choice_button\.top_margin\s*=\s*(\d+)", f, "choice_margin", FmtRaw);
                Grab(ln, @"style\.menu_window\.yalign\s*=\s*([0-9]*\.?[0-9]+)", f, "menu_yalign", FmtRaw);

                // ---- 7.x/8.x gui.* dialect ----
                Grab(ln, @"gui\.text_size\s*=\s*(\d+)", f, "text_size", FmtRaw);
                Grab(ln, @"gui\.text_font\s*=\s*(.+)", f, "text_font", FmtName);
                Grab(ln, @"gui\.(?:dialogue_)?text_color\s*=\s*(.+)", f, "text_color", FmtColor);
                Grab(ln, @"gui\.name_text_size\s*=\s*(\d+)", f, "name_size", FmtRaw);
                Grab(ln, @"gui\.textbox_height\s*=\s*(\d+)", f, "textbox_height", FmtRaw);

                // ---- per-character name colour ----
                MatchCharColor(ln, f);
            }

            // ---- shared in-game ("MMO chat") textbox (Character(None, window_background=Image(..),
            // window_*_padding=.., what_font=.., what_size=..)). These chat characters all share one
            // fixed-size box + small font, distinct from the main ADV textbox; extract it once. ----
            ExtractIngameBox(p, f);

            // ---- classic main/game menu membership + order (config.main_menu / config.game_menu) ----
            ExtractMenuOrder(lines, f);

            // ---- per-character side image (show_side_image=ConditionSwitch) + namebox (show_two_window) ----
            ExtractSideImages(p, f);
            ExtractTwoWindow(lines, p, f);

            // menu choice text colour: menu_choice = Style(style.default) (00style.rpy), so it inherits
            // style.default.color (= text_color). Classic roundrect themes change the HOVER via the button
            // background, not the text colour, so idle == hover. Only fill these if the game didn't set an
            // explicit menu_choice colour above.
            if (!f.ContainsKey("choice_color") && f.ContainsKey("text_color"))
                f["choice_color"] = f["text_color"];
            if (!f.ContainsKey("choice_hover_color") && f.ContainsKey("choice_color"))
                f["choice_hover_color"] = f["choice_color"];

            // For gui.* games the textbox is conventionally gui/textbox.png; only assume it if
            // we found gui.* settings and no explicit 6.x window background.
            if (!f.ContainsKey("textbox_bg") && (f.ContainsKey("textbox_height") || HasGui(lines)))
                f["textbox_bg"] = "textbox.png";

            // Deterministic output: sorted simple keys first, then sorted char colours.
            List<string> keys = new List<string>(f.Keys);
            keys.Sort(System.StringComparer.Ordinal);
            StringBuilder sb = new StringBuilder();
            foreach (string k in keys)
                if (!k.StartsWith("char_color.")) sb.Append(k).Append('=').Append(f[k]).Append('\n');
            foreach (string k in keys)
                if (k.StartsWith("char_color.")) sb.Append(k).Append('=').Append(f[k]).Append('\n');
            return sb.ToString();
        }

        private static bool HasGui(List<string> lines)
        {
            foreach (string l in lines) if (l.StartsWith("gui.")) return true;
            return false;
        }

        // Reconstruct config.main_menu / config.game_menu MEMBERSHIP + ORDER. They start as the engine
        // defaults (renpy/common/00layout.rpy) and a game mutates them (e.g. broken_memories does
        // `config.main_menu.insert(2, ('Gallery', ...))`). We model the engine default then replay the
        // game's insert/append/remove so the player renders the game's actual buttons in order -- no
        // hardcoded per-game list on the console. Emitted as mm_order / gm_order (pipe-joined labels).
        // The label -> action mapping stays engine-constant in the player; unknown labels (Gallery) are
        // custom game screens the player shows as inert. Help is dropped when config.help is disabled.
        private static void ExtractMenuOrder(List<string> lines, Dictionary<string, string> f)
        {
            List<string> main = new List<string> { "Start Game", "Load Game", "Preferences", "Help", "Quit" };
            List<string> game = new List<string> { "Return", "Preferences", "Save Game", "Load Game", "Main Menu", "Help", "Quit" };
            // The Help entry's engine visible-condition is `config.help`, which defaults to None (hidden).
            // Only keep Help if the game sets config.help to a truthy value (a non-empty string).
            bool helpOn = false;
            foreach (string ln in lines)
            {
                if (Regex.IsMatch(ln, @"config\.help\s*=\s*[""'][^""']")) helpOn = true;   // config.help = "README.html"
                ApplyMenuMutation(ln, "main_menu", main);
                ApplyMenuMutation(ln, "game_menu", game);
            }
            if (!helpOn) { main.Remove("Help"); game.Remove("Help"); }

            f["mm_order"] = string.Join("|", main.ToArray());
            f["gm_order"] = string.Join("|", game.ToArray());
        }

        private static void ApplyMenuMutation(string ln, string which, List<string> list)
        {
            // config.<which>.insert(N, ('Label', ...))
            Match m = Regex.Match(ln, @"config\." + which + @"\.insert\(\s*(\d+)\s*,\s*\(\s*u?[""']([^""']+)[""']");
            if (m.Success)
            {
                int idx = int.Parse(m.Groups[1].Value);
                if (idx < 0) idx = 0; if (idx > list.Count) idx = list.Count;
                if (!list.Contains(m.Groups[2].Value)) list.Insert(idx, m.Groups[2].Value);
                return;
            }
            // config.<which>.append(('Label', ...))
            m = Regex.Match(ln, @"config\." + which + @"\.append\(\s*\(\s*u?[""']([^""']+)[""']");
            if (m.Success) { if (!list.Contains(m.Groups[1].Value)) list.Add(m.Groups[1].Value); return; }
            // config.<which>.remove((... 'Label' ...)) or a bare label
            m = Regex.Match(ln, @"config\." + which + @"\.remove\([^)]*[""']([^""']+)[""']");
            if (m.Success) { list.Remove(m.Groups[1].Value); return; }
        }

        // The shared in-game chat textbox: the first Character(...) call that carries a
        // window_background (the MMO-chat speakers in e.g. RE:Alistair). All such characters share
        // one fixed-size box image + paddings + small `what` font/size, so one extraction covers them.
        // Emitted as ig_textbox_bg / ig_pad_l/_r/_b/_t / ig_font / ig_size (native px; basenames).
        private static void ExtractIngameBox(IrProgram p, Dictionary<string, string> f)
        {
            foreach (string s in p.Strings)
            {
                if (s == null) continue;
                int idx = 0;
                while ((idx = s.IndexOf("Character(", idx, System.StringComparison.Ordinal)) >= 0)
                {
                    int open = idx + "Character".Length;   // the '(' of this call
                    string call = BalancedCall(s, open);
                    idx += 1;
                    if (call.IndexOf("window_background", System.StringComparison.Ordinal) < 0) continue;

                    // window_background = Image("GUI/gtextbox.png", ...) (or Frame()). Drawn at its
                    // NATURAL size (Image) in the player; the file basename is enough.
                    Match bg = Regex.Match(call, @"window_background\s*=\s*(?:Image|Frame)\(\s*[""']([^""']+)[""']");
                    if (!bg.Success) continue;
                    f["ig_textbox_bg"] = BaseName(bg.Groups[1].Value);
                    GrabInt(call, @"window_left_padding\s*=\s*(\d+)",   f, "ig_pad_l");
                    GrabInt(call, @"window_right_padding\s*=\s*(\d+)",  f, "ig_pad_r");
                    GrabInt(call, @"window_bottom_padding\s*=\s*(\d+)", f, "ig_pad_b");
                    GrabInt(call, @"window_top_padding\s*=\s*(\d+)",    f, "ig_pad_t");
                    // The box image's own xalign/yalign within the window (don't assume bottom-left).
                    Match ax = Regex.Match(call, @"window_background\s*=\s*(?:Image|Frame)\([^)]*\bxalign\s*=\s*([0-9]*\.?[0-9]+)");
                    Match ay = Regex.Match(call, @"window_background\s*=\s*(?:Image|Frame)\([^)]*\byalign\s*=\s*([0-9]*\.?[0-9]+)");
                    if (ax.Success) f["ig_align_x"] = ax.Groups[1].Value;
                    if (ay.Success) f["ig_align_y"] = ay.Groups[1].Value;
                    Match wf = Regex.Match(call, @"what_font\s*=\s*[""']([^""']+)[""']");
                    if (wf.Success) f["ig_font"] = BaseName(wf.Groups[1].Value);
                    GrabInt(call, @"what_size\s*=\s*(\d+)", f, "ig_size");
                    // The chat text's own drop shadow (what_drop_shadow + colour); NOT the game's global one.
                    Match sh = Regex.Match(call, @"what_drop_shadow\s*=\s*\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)");
                    if (sh.Success) f["ig_shadow"] = sh.Groups[1].Value + "," + sh.Groups[2].Value;
                    Match sc = Regex.Match(call, @"what_drop_shadow_color\s*=\s*[""'](#[0-9A-Fa-f]{3,8})[""']");
                    if (sc.Success) f["ig_shadow_color"] = ColorA(sc.Groups[1].Value);
                    // The chat Characters only override left/right/bottom padding + background; they
                    // INHERIT style.window.top_padding (and yminimum + margins, which the player reuses
                    // from the main window). Carry the inherited top_padding so the player can place the
                    // text at the right height via the engine's Window box model (no per-game fraction).
                    if (!f.ContainsKey("ig_pad_t"))
                    {
                        if (f.ContainsKey("textbox_pad_t")) f["ig_pad_t"] = f["textbox_pad_t"];
                        else if (f.ContainsKey("textbox_pad_y")) f["ig_pad_t"] = f["textbox_pad_y"];
                    }
                    return;   // first chat box wins (shared across all chat characters)
                }
            }
        }

        private static void GrabInt(string s, string rx, Dictionary<string, string> f, string key)
        {
            Match m = Regex.Match(s, rx);
            if (m.Success) f[key] = m.Groups[1].Value;
        }

        // Per-character SIDE IMAGE: a Character defined with show_side_image=ConditionSwitch("cond","img",
        // ..., xalign=, yalign=). Ren'Py draws the first true condition's image beside the textbox while
        // that character speaks (character.py show_display_say -> ui.image(side_image)); ConditionSwitch
        // picks by evaluating each condition in order. We bake, keyed by the speaker's DISPLAY name (the
        // player matches it to the current speaker): the ConditionSwitch xalign/yalign and a pipe-list of
        // exprId:imageBasename pairs. Each condition is compiled to an RPN expr (same machinery as if/menu
        // guards) so the player evaluates it generically against the live vars -- no per-game assumption
        // about which variable or form the condition uses. Emitted as:
        //   side_image.<DisplayName> = <xalign>,<yalign>;<exprId>:<img>|<exprId>:<img>|...
        private static void ExtractSideImages(IrProgram p, Dictionary<string, string> f)
        {
            foreach (string s in p.Strings)
            {
                if (s == null) continue;
                int idx = 0;
                while ((idx = s.IndexOf("Character(", idx, System.StringComparison.Ordinal)) >= 0)
                {
                    int open = idx + "Character".Length;
                    string call = BalancedCall(s, open);
                    idx += 1;
                    int si = call.IndexOf("show_side_image", System.StringComparison.Ordinal);
                    if (si < 0) continue;
                    int cs = call.IndexOf("ConditionSwitch", si, System.StringComparison.Ordinal);
                    if (cs < 0) continue;
                    // The speaker display name is the Character's first positional arg.
                    Match nameM = Regex.Match(call, @"^\(\s*u?[""']([^""']+)[""']");
                    if (!nameM.Success) continue;   // Character(None,..) has no side-image name to key on
                    string name = nameM.Groups[1].Value;

                    int csOpen = call.IndexOf('(', cs);
                    if (csOpen < 0) continue;
                    string sw = BalancedCall(call, csOpen);   // "(...)" of ConditionSwitch

                    // Quoted tokens alternate cond,img,cond,img,...; trailing xalign/yalign are bare kwargs.
                    MatchCollection toks = Regex.Matches(sw, @"[""']([^""']*)[""']");
                    StringBuilder pairs = new StringBuilder();
                    for (int k = 0; k + 1 < toks.Count; k += 2)
                    {
                        string cond = toks[k].Groups[1].Value;
                        string img  = toks[k + 1].Groups[1].Value;
                        int exprId = p.CompileExpr(cond);
                        if (exprId < 0) continue;   // unsupported condition -> skip (don't fake always-true)
                        if (pairs.Length > 0) pairs.Append('|');
                        pairs.Append(exprId).Append(':').Append(BaseName(img));
                    }
                    if (pairs.Length == 0) continue;
                    Match ax = Regex.Match(sw, @"\bxalign\s*=\s*([0-9]*\.?[0-9]+)");
                    Match ay = Regex.Match(sw, @"\byalign\s*=\s*([0-9]*\.?[0-9]+)");
                    string xa = ax.Success ? ax.Groups[1].Value : "0";      // ConditionSwitch default pos = top-left,
                    string ya = ay.Success ? ay.Groups[1].Value : "0";      // but games set yalign 1.0 (bottom)
                    f["side_image." + name] = xa + "," + ya + ";" + pairs.ToString();
                }
            }
        }

        // show_two_window NAMEBOX: characters defined with show_two_window=True put the speaker NAME in a
        // separate say_who_window box (classic two-window say), not inline in the dialogue box. Collect the
        // display names of such characters, and extract the say_who_window style the game sets (background
        // image + position/anchor + padding). The player draws the name in this box for those speakers.
        //   two_window_names = Name|Name|...
        //   who_bg, who_xpos, who_ypos, who_xanchor, who_yanchor, who_lpad, who_tpad
        private static void ExtractTwoWindow(List<string> lines, IrProgram p, Dictionary<string, string> f)
        {
            List<string> names = new List<string>();
            foreach (string s in p.Strings)
            {
                if (s == null) continue;
                int idx = 0;
                while ((idx = s.IndexOf("Character(", idx, System.StringComparison.Ordinal)) >= 0)
                {
                    int open = idx + "Character".Length;
                    string call = BalancedCall(s, open);
                    idx += 1;
                    if (!Regex.IsMatch(call, @"show_two_window\s*=\s*True")) continue;
                    Match nameM = Regex.Match(call, @"^\(\s*u?[""']([^""']+)[""']");
                    if (nameM.Success && !names.Contains(nameM.Groups[1].Value)) names.Add(nameM.Groups[1].Value);
                }
            }
            if (names.Count > 0) f["two_window_names"] = string.Join("|", names.ToArray());

            // say_who_window style overrides (classic two-window namebox), from style.say_who_window.* lines.
            foreach (string ln in lines)
            {
                Grab(ln, @"style\.say_who_window\.background\s*=\s*(.+)", f, "who_bg", FmtName);
                GrabInt(ln, @"style\.say_who_window\.xpos\s*=\s*(-?\d+)",         f, "who_xpos");
                GrabInt(ln, @"style\.say_who_window\.ypos\s*=\s*(-?\d+)",         f, "who_ypos");
                GrabInt(ln, @"style\.say_who_window\.left_padding\s*=\s*(\d+)",   f, "who_lpad");
                GrabInt(ln, @"style\.say_who_window\.top_padding\s*=\s*(\d+)",    f, "who_tpad");
                GrabFloat(ln, @"style\.say_who_window\.xanchor\s*=\s*([0-9]*\.?[0-9]+)", f, "who_xanchor");
                GrabFloat(ln, @"style\.say_who_window\.yanchor\s*=\s*([0-9]*\.?[0-9]+)", f, "who_yanchor");
            }
        }

        private static void GrabFloat(string s, string rx, Dictionary<string, string> f, string key)
        {
            Match m = Regex.Match(s, rx);
            if (m.Success) f[key] = m.Groups[1].Value;
        }

        // The full balanced (...) call text within `s` starting at the '(' index.
        private static string BalancedCall(string s, int openIdx)
        {
            int depth = 0;
            for (int k = openIdx; k < s.Length; k++)
            {
                if (s[k] == '(') depth++;
                else if (s[k] == ')') { depth--; if (depth == 0) return s.Substring(openIdx, k - openIdx + 1); }
            }
            return s.Substring(openIdx);
        }

        // style.window.background = Frame("img", L, T) or Frame("img", L, T, R, B).
        // Accepts single or double quotes and an im.Image("...") wrapper around the file.
        private static void MatchFrame(string ln, Dictionary<string, string> f)
        {
            Match m = Regex.Match(ln,
                @"style\.(?:say_)?window\.background\s*=\s*Frame\(\s*(?:im\.Image\(\s*)?[""']([^""']+)[""']\)?\s*,\s*(\d+)\s*,\s*(\d+)(?:\s*,\s*(\d+)\s*,\s*(\d+))?");
            if (!m.Success) return;
            f["textbox_bg"] = BaseName(m.Groups[1].Value);
            f.Remove("textbox_color");   // an image background replaces any solid colour
            if (m.Groups[4].Success)
                f["textbox_frame"] = m.Groups[2].Value + "," + m.Groups[3].Value + "," + m.Groups[4].Value + "," + m.Groups[5].Value;
            else
                f["textbox_frame"] = m.Groups[2].Value + "," + m.Groups[3].Value + "," + m.Groups[2].Value + "," + m.Groups[3].Value;
        }

        // style.menu_choice_button.<which> = Frame("img", L, T[, R, B]) -> imgKey (basename) + frameKey
        // (l,t,r,b 9-slice insets). Falls back to a plain Image("img")/"img" string (no insets) if not a Frame.
        private static void MatchChoiceFrame(string ln, string which, Dictionary<string, string> f, string imgKey, string frameKey)
        {
            Match m = Regex.Match(ln,
                @"style\.menu_choice_button\." + which + @"\s*=\s*Frame\(\s*(?:im\.Image\(\s*)?[""']([^""']+)[""']\)?\s*,\s*(\d+)\s*,\s*(\d+)(?:\s*,\s*(\d+)\s*,\s*(\d+))?");
            if (m.Success)
            {
                f[imgKey] = BaseName(m.Groups[1].Value);
                if (m.Groups[4].Success)
                    f[frameKey] = m.Groups[2].Value + "," + m.Groups[3].Value + "," + m.Groups[4].Value + "," + m.Groups[5].Value;
                else
                    f[frameKey] = m.Groups[2].Value + "," + m.Groups[3].Value + "," + m.Groups[2].Value + "," + m.Groups[3].Value;
                return;
            }
            Match im = Regex.Match(ln, @"style\.menu_choice_button\." + which + @"\s*=\s*(?:Image\(\s*)?[""']([^""']+)[""']");
            if (im.Success) f[imgKey] = BaseName(im.Groups[1].Value);
        }

        // style.window.background as a SOLID colour: "#rgb"/"#rrggbb"/"#rrggbbaa" literal or
        // Solid((r, g, b, a)). Emitted as textbox_color (and clears any image background).
        private static void MatchWindowColor(string ln, Dictionary<string, string> f)
        {
            Match m = Regex.Match(ln, @"style\.(?:say_)?window\.background\s*=\s*[""'](#[0-9A-Fa-f]{3,8})[""']");
            string col = m.Success ? ColorA(m.Groups[1].Value) : "";
            if (col.Length == 0)
            {
                m = Regex.Match(ln, @"style\.(?:say_)?window\.background\s*=\s*Solid\(\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*(\d+)\s*)?\)");
                if (m.Success)
                {
                    int r = int.Parse(m.Groups[1].Value), g = int.Parse(m.Groups[2].Value), b = int.Parse(m.Groups[3].Value);
                    int a = m.Groups[4].Success ? int.Parse(m.Groups[4].Value) : 255;
                    col = string.Format("#{0:x2}{1:x2}{2:x2}{3:x2}", r & 255, g & 255, b & 255, a & 255);
                }
            }
            if (col.Length == 0) return;
            f["textbox_color"] = col;
            f.Remove("textbox_bg");
            f.Remove("textbox_frame");
        }

        // Text wrap uses FRACTIONAL advance accumulation in EVERY Ren'Py era, not per-glyph ceil.
        // Verified from the engine source: renpy/display/text.py get_width() -> self.f.size(text)[0]
        // (the font measures a string by summing its fractional advances), in both the SDL_ttf era
        // (6.x, e.g. Alistair 6.10.2e) and the ftfont era (6.13+). The earlier `ceil` heuristic was an
        // incorrect assumption that SDL_ttf ceils each advance; it over-counted and wrapped lines early
        // (proven on Alistair toony_loons.otf: ceil 416px > the 410px area while the real fractional
        // sum is 397.9px and reproduces the PC line breaks exactly). So always emit "float" -> the
        // player uses FONT_METRICS_NATIVE (fractional). (A `ceil` value remains supported by the player
        // for any future font that genuinely needs integer advances, e.g. a BMFont.)
        private static void MatchScriptVersion(string ln, Dictionary<string, string> f)
        {
            Match m = Regex.Match(ln, @"config\.script_version\s*=\s*\(?\s*(\d+)\s*,\s*(\d+)");
            if (!m.Success) return;
            f["text_advance"] = "float";
        }

        // style.window.ymargin = N sets BOTH the top and bottom margin (Ren'Py's ymargin is
        // a shorthand for top_margin + bottom_margin).
        private static void MatchYmargin(string ln, Dictionary<string, string> f)
        {
            Match m = Regex.Match(ln, @"style\.(?:say_)?window\.ymargin\s*=\s*(\d+)");
            if (!m.Success) return;
            f["textbox_margin_t"] = m.Groups[1].Value;
            f["textbox_margin_b"] = m.Groups[1].Value;
        }

        // <var> = Character("Display", ... color="#rrggbb" ...) -> char_color.Display = #rrggbb
        private static void MatchCharColor(string ln, Dictionary<string, string> f)
        {
            Match m = Regex.Match(ln, @"=\s*Character\(\s*[""']([^""']+)[""'][^)]*\bcolor\s*=\s*[""'](#[0-9A-Fa-f]{3,8})[""']");
            if (!m.Success) return;
            string name = m.Groups[1].Value;
            string col = Color(m.Groups[2].Value);
            if (!string.IsNullOrEmpty(col)) f["char_color." + name] = col;
        }

        // A theme.roundrect colour kwarg on its own line: `<name> = (r,g,b[,a])` or `<name> = "#hex"`.
        // Stores it as #rrggbbaa under key (the player's parseColorAlpha reads it). Alpha defaults 255.
        private static void MatchThemeColor(string ln, string name, Dictionary<string, string> f, string key)
        {
            Match t = Regex.Match(ln, @"^\s*" + name + @"\s*=\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*(\d+)\s*)?\)");
            if (t.Success)
            {
                int r = int.Parse(t.Groups[1].Value), g = int.Parse(t.Groups[2].Value), b = int.Parse(t.Groups[3].Value);
                int a = t.Groups[4].Success ? int.Parse(t.Groups[4].Value) : 255;
                f[key] = string.Format("#{0:x2}{1:x2}{2:x2}{3:x2}", r & 255, g & 255, b & 255, a & 255);
                return;
            }
            Match h = Regex.Match(ln, @"^\s*" + name + @"\s*=\s*[""'](#[0-9A-Fa-f]{3,8})[""']");
            if (h.Success) f[key] = ColorA(h.Groups[1].Value);
        }

        private const int FmtRaw = 0, FmtName = 1, FmtColor = 2, FmtColorA = 3;

        // Matches rx (one capture group) and stores the formatted value under key (last wins).
        private static void Grab(string ln, string rx, Dictionary<string, string> f, string key, int fmt)
        {
            Match m = Regex.Match(ln, rx);
            if (!m.Success) return;
            string v = m.Groups[1].Value.Trim();
            if (fmt == FmtName) v = BaseName(Unq(v));
            else if (fmt == FmtColor) v = Color(v);
            else if (fmt == FmtColorA) v = ColorA(v);
            if (!string.IsNullOrEmpty(v)) f[key] = v;
        }

        // Matches rx (two capture groups) into two keys (used for gui.init(W,H)).
        private static void MatchTwo(string ln, string rx, Dictionary<string, string> f, string k1, string k2)
        {
            Match m = Regex.Match(ln, rx);
            if (!m.Success) return;
            f[k1] = m.Groups[1].Value.Trim();
            f[k2] = m.Groups[2].Value.Trim();
        }

        // Matches rx (two capture groups) and joins them "a,b" under key (used for drop_shadow).
        private static void MatchTwoJoin(string ln, string rx, Dictionary<string, string> f, string key)
        {
            Match m = Regex.Match(ln, rx);
            if (m.Success) f[key] = m.Groups[1].Value.Trim() + "," + m.Groups[2].Value.Trim();
        }

        private static string Unq(string s)
        {
            s = s.Trim().TrimEnd(',', ')', ' ');
            if (s.Length >= 2 && (s[0] == '"' || s[0] == '\'') && s[s.Length - 1] == s[0]) return s.Substring(1, s.Length - 2);
            return s;
        }

        // Normalizes a colour literal ("#rgb"/"#rrggbb"/"#rrggbbaa") to #rrggbb. Non-literals
        // (variables/expressions) return "" so we don't emit a bogus value.
        private static string Color(string v)
        {
            v = Unq(v);
            Match m = Regex.Match(v, @"^#([0-9A-Fa-f]{3,8})$");
            if (!m.Success) return "";
            string h = m.Groups[1].Value;
            if (h.Length == 3) h = "" + h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
            if (h.Length >= 6) return "#" + h.Substring(0, 6).ToLowerInvariant();
            return "";
        }

        // Like Color() but PRESERVES alpha, normalizing Ren'Py's #rgb/#rgba/#rrggbb/#rrggbbaa
        // shorthand to #rrggbb[aa] (the player's parseColorAlpha reads either).
        private static string ColorA(string v)
        {
            v = Unq(v);
            Match m = Regex.Match(v, @"^#([0-9A-Fa-f]{3,8})$");
            if (!m.Success) return "";
            string h = m.Groups[1].Value.ToLowerInvariant();
            if (h.Length == 3 || h.Length == 4)
            {
                StringBuilder sb = new StringBuilder();
                foreach (char c in h) { sb.Append(c); sb.Append(c); }
                h = sb.ToString();
            }
            if (h.Length == 6 || h.Length == 8) return "#" + h;
            return "";
        }

        private static string BaseName(string path)
        {
            if (string.IsNullOrEmpty(path)) return path;
            int slash = path.Replace('\\', '/').LastIndexOf('/');
            return slash >= 0 ? path.Substring(slash + 1) : path;
        }
    }
}
