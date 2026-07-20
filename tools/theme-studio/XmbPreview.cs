using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace ThemeStudio
{
   // draws a rough XMB over the chosen background so icons can be judged against it.
   // deliberately a placeholder, not a facsimile: it answers "do my icons read here?",
   // which is the question the compilers cannot answer and the console answers too slowly.
   public static class XmbPreview
   {
      public const double ScreenWidth = 1920;
      public const double ScreenHeight = 1080;

      // where the horizontal row crosses the vertical column
      private const double CrossX = 360;
      private const double CrossY = 430;
      private const double RowSpacing = 190;
      private const double ColumnSpacing = 135;
      // clears the selected item's label, which sits directly below the cross
      private const double ColumnStart = 195;
      private const double IconSize = 100;
      private const double SelectedIconSize = 128;

      // each main-row entry opens a column of items; these are the groups they map to
      private static readonly Dictionary<string, string> ColumnGroupByRowId = new Dictionary<string, string> {
         { "icon_user", "Users" }, { "icon_setting", "Settings" }, { "icon_photo", "Media" },
         { "icon_music", "Media" }, { "icon_video", "Media" }, { "icon_game", "Game" },
         { "icon_network", "Network" }, { "icon_friend", "Friends" }
      };

      // the console's own three menu faces, in the order the theme picks them: standard, rounded
      // and pop. the files sit in the program's own assets folder, so the preview shows the real
      // lettering rather than a lookalike.
      private static readonly string[] MenuFontNames = {
         "SCE-PS3 Rodin LATIN", "SCE-PS3 Seurat LATIN", "VAGRundschriftDLig"
      };
      private static readonly FontFamily[] menuFonts = new FontFamily[MenuFontNames.Length];
      private static FontFamily menuFont = getMenuFont(0);

      private static FontFamily getMenuFont(int selection)
      {
         if (menuFonts[selection] == null) {
            var folder = new Uri(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "assets", "fonts") +
                                 Path.DirectorySeparatorChar);
            menuFonts[selection] = new FontFamily(folder, "./#" + MenuFontNames[selection]);
         }
         return menuFonts[selection];
      }

      public static void Render(Canvas canvas, ThemeProject project, string selectedRowId, UIElement scene)
      {
         menuFont = getMenuFont(project.FontSelection >= 0 && project.FontSelection < MenuFontNames.Length
            ? project.FontSelection : 0);
         canvas.Children.Clear();
         canvas.Width = ScreenWidth;
         canvas.Height = ScreenHeight;

         drawBackground(canvas, project, scene);
         // icons draw over whatever the background turned out to be, exactly as on the console
         drawMainRow(canvas, project, selectedRowId);
         drawColumn(canvas, project, selectedRowId);
      }

      // background

      private static void drawBackground(Canvas canvas, ThemeProject project, UIElement scene)
      {
         Background background = project.Backgrounds.Count > 0 ? project.Backgrounds[0] : null;
         canvas.Background = new SolidColorBrush(Color.FromRgb(0x10, 0x14, 0x20));

         if (background == null) return;   // just the empty backdrop, no note
         if (background.IsProjectScene) {
            if (scene != null) canvas.Children.Add(scene);
            return;
         }
         // an already-built scene has no models or textures left in it to draw -- compiling threw
         // them away -- so there is nothing the editor could show even in principle
         if (background.IsScene) {
            addNote(canvas, "This background is an already-built 3D scene (" +
                            Path.GetFileName(background.ScenePath) + ").");
            addNote(canvas, "It will work on the console, but it cannot be shown here or edited.", 50);
            return;
         }

         BitmapImage image = ImageFile.Load(project.ResolveAsset(background.WidescreenPath));
         if (image == null) { addNote(canvas, "background image could not be read"); return; }
         canvas.Children.Add(new Image {
            Source = image, Width = ScreenWidth, Height = ScreenHeight, Stretch = Stretch.UniformToFill
         });
      }

      // the horizontal row of categories, selected one centred on the cross

      private static void drawMainRow(Canvas canvas, ThemeProject project, string selectedRowId)
      {
         List<IconSlot> row = getRowSlots();
         int selectedIndex = row.FindIndex(slot => slot.Id == selectedRowId);
         if (selectedIndex < 0) selectedIndex = 0;

         for (int index = 0; index < row.Count; index++) {
            bool isSelected = index == selectedIndex;
            double size = isSelected ? SelectedIconSize : IconSize;
            double centreX = CrossX + (index - selectedIndex) * RowSpacing;
            if (centreX < -size || centreX > ScreenWidth + size) continue;

            addIcon(canvas, project, row[index], centreX, CrossY, size, isSelected ? 1.0 : 0.55);
            if (isSelected) addLabel(canvas, row[index].Label, centreX, CrossY + size / 2 + 12, 26, 1.0);
         }
      }

      // the vertical column belonging to the selected category

      private static void drawColumn(Canvas canvas, ThemeProject project, string selectedRowId)
      {
         string group;
         if (!ColumnGroupByRowId.TryGetValue(selectedRowId ?? "", out group)) return;

         double y = CrossY + ColumnStart;
         foreach (IconSlot slot in IconSlots.All) {
            if (slot.Group != group) continue;
            if (y > ScreenHeight - 60) break;
            addIcon(canvas, project, slot, CrossX, y, IconSize, 0.85);
            addLabel(canvas, slot.Label, CrossX + IconSize / 2 + 20, y, 22, 0.85, false);
            y += ColumnSpacing;
         }
      }

      // pieces

      private static void addIcon(Canvas canvas, ThemeProject project, IconSlot slot,
                                  double centreX, double centreY, double size, double opacity)
      {
         string storedPath;
         BitmapImage image = project.IconPaths.TryGetValue(slot.Id, out storedPath)
            ? ImageFile.Load(project.ResolveAsset(storedPath))
            : loadDefaultIcon(slot.Id);

         // an icon keeps its own aspect: the photo and video slots are wider than they are tall
         double width = size * slot.Width / slot.Height;

         FrameworkElement element;
         if (image != null) {
            element = new Image { Source = image, Width = width, Height = size, Stretch = Stretch.Uniform };
         } else {
            // no picture of any kind: a hollow box so the layout still reads
            element = new Border {
               Width = width, Height = size,
               BorderBrush = new SolidColorBrush(Color.FromArgb(0x88, 0xFF, 0xFF, 0xFF)),
               BorderThickness = new Thickness(2),
               Background = new SolidColorBrush(Color.FromArgb(0x22, 0xFF, 0xFF, 0xFF))
            };
         }
         element.Opacity = opacity;
         Canvas.SetLeft(element, centreX - width / 2);
         Canvas.SetTop(element, centreY - size / 2);
         canvas.Children.Add(element);
      }

      private static void addLabel(Canvas canvas, string text, double x, double y, double fontSize,
                                   double opacity, bool centred = true)
      {
         var label = new TextBlock {
            Text = text, FontSize = fontSize, FontFamily = menuFont,
            Foreground = Brushes.White, Opacity = opacity,
            Effect = new System.Windows.Media.Effects.DropShadowEffect {
               BlurRadius = 6, ShadowDepth = 1, Opacity = 0.9, Color = Colors.Black
            }
         };
         label.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
         Canvas.SetLeft(label, centred ? x - label.DesiredSize.Width / 2 : x);
         Canvas.SetTop(label, centred ? y : y - label.DesiredSize.Height / 2);
         canvas.Children.Add(label);
      }

      private static void addNote(Canvas canvas, string text, double below = 0)
      {
         addLabel(canvas, text, ScreenWidth / 2, ScreenHeight / 2 - 40 + below, 34, 0.75);
      }

      // a slot the user has not set keeps the console's own icon, so the preview shows one too.
      // these are Sony's own sample icons standing in for the console's -- close enough to judge
      // your own artwork against, but not pixel-for-pixel what the XMB draws.
      private static readonly Dictionary<string, BitmapImage> defaultIcons = new Dictionary<string, BitmapImage>();

      private static BitmapImage loadDefaultIcon(string id)
      {
         BitmapImage image;
         if (defaultIcons.TryGetValue(id, out image)) return image;

         string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "assets", "default-icons", id + ".png");
         image = ImageFile.Load(path);
         defaultIcons[id] = image;
         return image;
      }

      private static List<IconSlot> getRowSlots()
      {
         var row = new List<IconSlot>();
         foreach (IconSlot slot in IconSlots.All)
            if (slot.Group == "Main row") row.Add(slot);
         return row;
      }
   }
}
