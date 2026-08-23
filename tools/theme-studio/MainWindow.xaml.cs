using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using Microsoft.Win32;

namespace ThemeStudio
{
   // one menu colour as plain data. a combo box moves the chosen item's visual into its popup,
   // so an item built from live controls vanishes from the closed box the moment it is opened.
   public class ColourChoice
   {
      public Brush Swatch { get; private set; }
      public string Label { get; private set; }

      public ColourChoice(string colour, string label)
      {
         Swatch = colour == null ? null : new SolidColorBrush((Color)ColorConverter.ConvertFromString(colour));
         Label = label;
      }

      // what the row is called to anything reading the window rather than looking at it
      public override string ToString() { return Label; }
   }

   public partial class MainWindow : Window
   {
      private ThemeProject project = new ThemeProject();
      private readonly Dictionary<string, Image> iconThumbnails = new Dictionary<string, Image>();
      private readonly Dictionary<string, Button> iconClearButtons = new Dictionary<string, Button>();
      // the slot cards are built once, so each keeps a way to show whatever the open project holds
      private readonly List<Action> slotRefreshers = new List<Action>();
      private readonly List<string> previewRowIds = new List<string>();
      private int previewRow;
      private readonly PsjsEditor scriptEditor;

      private const double CardWidth = 460;   // matches CardWidth in Theme.xaml

      private static readonly Brush CardBrush = new SolidColorBrush(Color.FromRgb(0x25, 0x25, 0x26));
      private static readonly Brush SurfaceBrush = new SolidColorBrush(Color.FromRgb(0x2D, 0x2D, 0x2D));
      private static readonly Brush EdgeBrush = new SolidColorBrush(Color.FromRgb(0x3F, 0x3F, 0x3F));
      private static readonly Brush GoodBrush = new SolidColorBrush(Color.FromRgb(0x6A, 0xB0, 0x4A));
      private static readonly Brush BadBrush = new SolidColorBrush(Color.FromRgb(0xD0, 0x6A, 0x5A));
      private static readonly Brush DimBrush = new SolidColorBrush(Color.FromRgb(0x8A, 0x8A, 0x8A));
      private static readonly Brush ClearCrossBrush = new SolidColorBrush(Color.FromRgb(0x8A, 0x3A, 0x32));
      private static readonly Brush HoverBrush = new SolidColorBrush(Color.FromRgb(0x3E, 0x3E, 0x3E));

      public MainWindow()
      {
         InitializeComponent();
         AppSettings.Load();
         ProjectPackage.SweepLeftovers();
         scriptEditor = new PsjsEditor(scriptBox, delegate { return PsjsSnippets.GetApiNames(scene.Actors); });
         buildColourList();
         ps3AddressBox.Text = AppSettings.Ps3Ip;
         buildIconGrid();
         buildSlotPanels();
         foreach (IconSlot slot in IconSlots.All)
            if (slot.Group == "Main row") previewRowIds.Add(slot.Id);
         previewRow = 5;   // Game, so the preview opens on a populated column
         showProject();
         openFromCommandLine();
      }

      // lets a .themeproj be opened by double-click, and gives the ui tests something to load
      private void openFromCommandLine()
      {
         string[] arguments = Environment.GetCommandLineArgs();
         if (arguments.Length < 2 || !File.Exists(arguments[1])) return;
         try {
            project = ThemeProject.Load(arguments[1]);
            showProject();
            log("opened " + arguments[1]);
         } catch (Exception exception) {
            log("could not open " + arguments[1] + ": " + exception.Message);
         }
      }

      // project <-> form

      private void showProject()
      {
         nameBox.Text = project.Name;
         authorBox.Text = project.Author;
         versionBox.Text = project.Version;
         commentBox.Text = project.Comment;
         fontBox.SelectedIndex = clamp(project.FontSelection, 0, 2);
         colorBox.SelectedIndex = clamp(project.ColorSelection, 0, 12);
         showBackgrounds();
         foreach (IconSlot slot in IconSlots.All) showIconSlot(slot.Id);
         foreach (Action refresh in slotRefreshers) refresh();
         showScene();
         showScript();   // an opened project's own script replaces whatever was on screen
         showBackgroundKind();
         showProjectName();
         showScenePlaying(isPreviewShowing());
      }

      private bool isPreviewShowing()
      {
         return tabs != null && tabs.SelectedItem == previewTab;
      }

      private void readProject()
      {
         project.Name = nameBox.Text.Trim();
         project.Author = authorBox.Text.Trim();
         project.Version = versionBox.Text.Trim();
         project.Comment = commentBox.Text.Trim();
         project.FontSelection = Math.Max(0, fontBox.SelectedIndex);
         project.ColorSelection = Math.Max(0, colorBox.SelectedIndex);
         saveScript();   // the script is part of the project, so it is never left only in the text box
      }

      private void showBackgrounds()
      {
         int keepIndex = backgroundList.SelectedIndex;
         backgroundList.Items.Clear();
         foreach (Background background in project.Backgrounds)
            backgroundList.Items.Add(makeBackgroundRow(background));

         // something is always selected, so the preview beside the list is never blank for no reason
         if (keepIndex >= 0 && keepIndex < backgroundList.Items.Count) backgroundList.SelectedIndex = keepIndex;
         else if (backgroundList.Items.Count > 0) backgroundList.SelectedIndex = 0;

         bool scheduled = project.BackgroundShowType.Length > 0 && project.Backgrounds.Count > 0;
         coverageLabel.Visibility = scheduled ? Visibility.Visible : Visibility.Collapsed;
         if (scheduled)
            coverageLabel.Text = BackgroundTiming.DescribeCoverage(project.Backgrounds,
                                                                   project.BackgroundShowType == "days");
      }

      // the picture on the left, when it shows on the right: the name is what is being picked from,
      // and the timing is a note about it rather than part of its identity
      private DockPanel makeBackgroundRow(Background background)
      {
         string name = background.IsScene ? "3D scene   " + Path.GetFileName(background.ScenePath)
                                          : Path.GetFileName(background.WidescreenPath);
         var row = new DockPanel();
         if (project.BackgroundShowType.Length > 0 && !background.IsScene) {
            var when = new TextBlock {
               Text = BackgroundTiming.Describe(background, project.BackgroundShowType == "days"),
               Foreground = DimBrush, FontSize = 11, Margin = new Thickness(12, 0, 0, 0)
            };
            DockPanel.SetDock(when, Dock.Right);
            row.Children.Add(when);
         }
         row.Children.Add(new TextBlock { Text = name, TextTrimming = TextTrimming.CharacterEllipsis });
         return row;
      }

      // preview

      private ScenePlayer scenePlayer;

      private void refreshPreview()
      {
         if (previewCanvas == null) return;
         XmbPreview.Render(previewCanvas, project, previewRowIds[previewRow],
                           scenePlayer == null ? null : scenePlayer.Viewport);
         showSceneHelpers();
      }

      // the light dots and the camera reading only mean anything while a scene is playing.
      // the checkbox raises Checked as the window is still being built, so this runs before
      // the rest of the tree exists.
      private void showSceneHelpers()
      {
         if (markersToggle == null || cameraReadout == null) return;
         bool playing = scenePlayer != null;
         markersToggle.Visibility = playing ? Visibility.Visible : Visibility.Collapsed;
         cameraReadout.Text = playing ? scenePlayer.GetCameraReadout() : "";
         if (playing) scenePlayer.ShowMarkers(markersToggle.IsChecked == true);
      }

      private void onMarkersToggled(object sender, RoutedEventArgs e) { showSceneHelpers(); }

      // a scene nobody is looking at is still drawn thirty times a second, and drawing it costs a
      // whole processor core -- so it runs only while this window is the one being used. it pauses
      // rather than stops, so coming back carries on from where the animation was.
      protected override void OnActivated(EventArgs e)
      {
         base.OnActivated(e);
         showScenePaused();
      }

      protected override void OnDeactivated(EventArgs e)
      {
         base.OnDeactivated(e);
         showScenePaused();
      }

      protected override void OnStateChanged(EventArgs e)
      {
         base.OnStateChanged(e);
         showScenePaused();
      }

      private void showScenePaused()
      {
         if (scenePlayer != null) scenePlayer.SetRunning(IsActive && WindowState != WindowState.Minimized);
      }

      // the scene starts playing when the preview comes into view and stops when it leaves, so it
      // always runs the script as written rather than as last saved, and costs nothing when hidden
      private void showScenePlaying(bool playing)
      {
         if (scenePlayer != null) { scenePlayer.Stop(); scenePlayer = null; }
         if (playing && findProjectSceneBackground() != null && project.Scene.Actors.Count > 0) {
            scenePlayer = ScenePlayer.Start(project, scriptBox.Text, XmbPreview.ScreenWidth,
                                            XmbPreview.ScreenHeight, log);
            scenePlayer.Ticked = showSceneHelpers;
         }
         refreshPreview();
      }

      // left and right move along the menu, the way the console's d-pad does. there is no up and
      // down: the column below simply belongs to whichever row entry is selected.
      private void onPreviewKeyDown(object sender, KeyEventArgs e)
      {
         if (e.Key != Key.Left && e.Key != Key.Right) return;
         previewRow += e.Key == Key.Right ? 1 : -1;
         if (previewRow < 0) previewRow = previewRowIds.Count - 1;
         if (previewRow >= previewRowIds.Count) previewRow = 0;
         refreshPreview();
         e.Handled = true;
      }

      private void onPreviewClicked(object sender, MouseButtonEventArgs e) { Keyboard.Focus(previewFrame); }

      // moving between stages

      private void onTabChanged(object sender, SelectionChangedEventArgs e)
      {
         if (e.OriginalSource != tabs || tabs.SelectedItem == null) return;
         readProject();
         bool showingPreview = tabs.SelectedItem == previewTab;
         showScenePlaying(showingPreview);
         if (showingPreview) focusPreview();
      }

      // the preview has to hold the keyboard, or the left and right arrows drive the tab strip
      // instead of the menu. the tab's content is not on screen yet when the tab changes, so the
      // focus is set once it is, and as keyboard focus rather than only logical focus.
      private void focusPreview()
      {
         Dispatcher.BeginInvoke(new Action(delegate { Keyboard.Focus(previewFrame); }),
                                System.Windows.Threading.DispatcherPriority.Input);
      }

      // backgrounds

      // on Checked rather than Click, so the keyboard moves between the two the same way the
      // mouse does. showBackgroundKind sets them itself, hence the guard.
      private bool settingBackgroundKind;

      private void onBackgroundKindChanged(object sender, RoutedEventArgs e)
      {
         if (settingBackgroundKind) return;
         bool moving = sceneToggle.IsChecked == true;
         Background projectScene = findProjectSceneBackground();
         if (moving && projectScene == null) {
            project.Backgrounds.Add(new Background { IsProjectScene = true });
         } else if (!moving && projectScene != null) {
            project.Backgrounds.Remove(projectScene);
         }
         showBackgroundKind();
         showBackgrounds();
         refreshPreview();
      }

      private void showBackgroundKind()
      {
         bool moving = findProjectSceneBackground() != null;
         settingBackgroundKind = true;
         sceneToggle.IsChecked = moving;
         picturesToggle.IsChecked = !moving;
         settingBackgroundKind = false;

         showOnly(moving, actorList, backgroundList);
         showOnly(moving, sceneDetail, picturesDetail);
         editButton.Visibility = moving ? Visibility.Visible : Visibility.Collapsed;
         primaryButtons.Columns = moving ? 3 : 2;
         // the script's own buttons live inside sceneDetail, so only this is left here
         showTypeBox.Visibility = showTypeLabel.Visibility = moving ? Visibility.Collapsed : Visibility.Visible;

         showTypeBox.SelectedIndex = project.BackgroundShowType == "days" ? 1
                                   : project.BackgroundShowType == "datetime" ? 2 : 0;
         showTiming();
      }

      // "shows from x until y" only means anything once the pictures take turns by date or hour
      private void showTiming()
      {
         Background background = selectedBackground();
         bool wanted = project.BackgroundShowType.Length > 0 && background != null && !background.IsScene;
         timingRow.Visibility = wanted ? Visibility.Visible : Visibility.Collapsed;
         if (!wanted) return;

         bool byDate = project.BackgroundShowType == "days";
         bool set = background.From.Length > 0 && background.Until.Length > 0;
         timingButton.Content = set ? "Change" : (byDate ? "Set dates" : "Set hours");
         timingHint.Text = set ? BackgroundTiming.Describe(background, byDate)
                               : "Not set, so this picture shows only when no other one is due.";
      }

      private void onSetTiming(object sender, RoutedEventArgs e)
      {
         Background background = selectedBackground();
         if (background == null) return;

         bool byDate = project.BackgroundShowType == "days";
         TimingWindow chosen = TimingWindow.Ask(this, byDate, background.From, background.Until);
         if (chosen == null) return;

         background.From = chosen.From;
         background.Until = chosen.Until;
         showBackgrounds();
         showTiming();
      }

      private Background selectedBackground()
      {
         int index = backgroundList.SelectedIndex;
         return index >= 0 && index < project.Backgrounds.Count ? project.Backgrounds[index] : null;
      }

      private static void showOnly(bool useFirst, UIElement first, UIElement second)
      {
         first.Visibility = useFirst ? Visibility.Visible : Visibility.Collapsed;
         second.Visibility = useFirst ? Visibility.Collapsed : Visibility.Visible;
      }

      // random needs nothing per picture; the other two need a window on each one
      private void onShowTypeChanged(object sender, SelectionChangedEventArgs e)
      {
         switch (showTypeBox.SelectedIndex) {
            case 1: project.BackgroundShowType = "days"; break;
            case 2: project.BackgroundShowType = "datetime"; break;
            default: project.BackgroundShowType = ""; break;
         }
         showBackgrounds();
         showTiming();
      }

      private void onBackgroundSelected(object sender, SelectionChangedEventArgs e)
      {
         Background background = selectedBackground();
         showTiming();
         backgroundPreview.Source = background == null || background.WidescreenPath.Length == 0
            ? null : ImageFile.Load(project.ResolveAsset(background.WidescreenPath));
         backgroundNote.Text = background == null ? "Add a picture to see it here."
                             : backgroundPreview.Source == null ? "This one has no picture to show." : "";
      }

      private Background findProjectSceneBackground()
      {
         foreach (Background background in project.Backgrounds)
            if (background.IsProjectScene) return background;
         return null;
      }

      // the twelve fixed menu colours, read off the figure in Sony's own specification
      private static readonly string[] ThemeColours = {
         "#747474", "#846A11", "#4E711C", "#965D6B", "#135112", "#684980",
         "#0B867F", "#123E73", "#7A3683", "#946E10", "#513915", "#8D2B1F"
      };

      private void buildColourList()
      {
         var choices = new List<ColourChoice> { new ColourChoice(null, "Changes with the time of day") };
         for (int index = 0; index < ThemeColours.Length; index++)
            choices.Add(new ColourChoice(ThemeColours[index], "Colour " + (index + 1)));
         colorBox.ItemsSource = choices;
         colorBox.SelectedIndex = 0;
      }

      // the simple slot lists: theme pictures, pointers and sounds

      private void buildSlotPanels()
      {
         WrapPanel pictures = addSection(picturePanel, "Theme pictures",
                    "How the theme presents itself on the console. Sizes must match exactly.");
         addPngCard(pictures, "Theme icon", "64x64, shown in the theme list",
                    delegate { return project.IconPath; }, delegate(string path) { project.IconPath = path; }, 64, 64);
         addPngCard(pictures, "Author icon", "64x64, shown beside your name",
                    delegate { return project.AuthorIconPath; }, delegate(string path) { project.AuthorIconPath = path; }, 64, 64);
         addPngCard(pictures, "Preview picture", "480x270, the large preview",
                    delegate { return project.PreviewPath; }, delegate(string path) { project.PreviewPath = path; }, 480, 270);
         addPngCard(pictures, "Notification frame", "64x64, the border around popup messages",
                    delegate { return project.NotificationPath; }, delegate(string path) { project.NotificationPath = path; }, 64, 64);

         WrapPanel pointers = addSection(pointerPanel, "Mouse pointers",
                    "Used by the web browser. 48x48 each. The numbers are the pixel in your picture " +
                    "that actually does the clicking.");
         foreach (PointerSlot slot in PointerSlots.All) addPathCard(pointers, slot);

         WrapPanel sounds = addSection(soundPanel, "Menu sounds",
                    "A .wav is converted to the console's own sound format when the theme is built. " +
                    "A .vag is used as it is.");
         foreach (SoundSlot slot in SoundSlots.All)
            addSlotCard(sounds, slot.Label, "", () => lookUp(project.SoundPaths, slot.Id),
                        "sounds (*.wav;*.vag)|*.wav;*.vag",
                        path => project.SoundPaths[slot.Id] = path,
                        () => project.SoundPaths.Remove(slot.Id), false, true);
      }

      private void addPngCard(Panel panel, string title, string hint, GetPath get, SetPath set, int width, int height)
      {
         addSlotCard(panel, title, hint, () => get(), "png images (*.png)|*.png", delegate(string path) {
            set(path);
            warnUnlessSize(path, width, height, title);
            refreshPreview();
         }, null);
      }

      private void addPathCard(Panel panel, PointerSlot slot)
      {
         string hint = "Clicks at " + slot.ClickX + ", " + slot.ClickY;
         addSlotCard(panel, slot.Label, hint, () => lookUp(project.PointerPaths, slot.Id),
                     "png images (*.png)|*.png", delegate(string path) {
                        project.PointerPaths[slot.Id] = path;
                        warnUnlessSize(path, PointerSlots.Size, PointerSlots.Size, slot.Label);
                     }, () => project.PointerPaths.Remove(slot.Id));
      }

      private static string lookUp(Dictionary<string, string> paths, string id)
      {
         string path;
         return paths.TryGetValue(id, out path) ? path : "";
      }

      private delegate string GetPath();
      private delegate void SetPath(string path);

      // one slot as a card: what it is on the left, the button that sets it on the right.
      // pictures, pointers and sounds are all this shape, so they all read the same.
      private void addSlotCard(Panel panel, string title, string hint, Func<string> readCurrent, string filter,
                               Action<string> chosen, Action cleared, bool showThumbnail = true,
                               bool isSound = false)
      {
         string current = readCurrent();
         string held = current;
         Func<string> getCurrent = delegate { return held; };
         Action<string> setCurrent = delegate(string value) { held = value; };
         var status = new TextBlock { Text = describeSlotFile(current), Foreground = DimBrush, FontSize = 11 };
         var thumbnail = new Image { Width = 56, Height = 56, Stretch = Stretch.Uniform,
                                     Margin = new Thickness(0, 0, 12, 0),
                                     Source = ImageFile.Load(project.ResolveAsset(current)) };
         var thumbnailFrame = new Border {
            Width = 58, Height = 58, Background = SurfaceBrush, BorderBrush = EdgeBrush,
            BorderThickness = new Thickness(1), Margin = new Thickness(0, 0, 12, 0), Child = thumbnail,
            Visibility = showThumbnail ? Visibility.Visible : Visibility.Collapsed
         };
         thumbnail.Margin = new Thickness(0);
         var button = new Button { Content = "Choose", Width = 90, Margin = new Thickness(0, 0, 8, 0) };
         // several buttons all called "Choose" are indistinguishable to anything reading the window
         AutomationProperties.SetName(button, "Choose " + title);

         var clearButton = new Button { Content = "Clear", Width = 70, Margin = new Thickness(0) };
         AutomationProperties.SetName(clearButton, "Clear " + title);
         clearButton.IsEnabled = !string.IsNullOrEmpty(current);

         var actions = new StackPanel { Orientation = Orientation.Horizontal,
                                        VerticalAlignment = VerticalAlignment.Center };
         if (isSound) {
            var playButton = new Button { Content = "Play", Width = 70, Margin = new Thickness(0, 0, 8, 0) };
            AutomationProperties.SetName(playButton, "Play " + title);
            playButton.Click += delegate { playSound(getCurrent()); };
            actions.Children.Add(playButton);
         }
         actions.Children.Add(button);
         actions.Children.Add(clearButton);

         var details = new StackPanel();
         details.Children.Add(new TextBlock { Text = title, Margin = new Thickness(0, 0, 0, 4) });
         if (hint.Length > 0)
            details.Children.Add(new TextBlock { Text = hint, Foreground = DimBrush, FontSize = 11,
                                                 TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 4) });
         details.Children.Add(status);

         var row = new DockPanel { LastChildFill = true };
         DockPanel.SetDock(actions, Dock.Right);
         DockPanel.SetDock(thumbnailFrame, Dock.Left);
         row.Children.Add(actions);
         row.Children.Add(thumbnailFrame);
         row.Children.Add(details);

         var card = new Border {
            Background = CardBrush, BorderBrush = EdgeBrush, BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(4), Padding = new Thickness(16), Width = CardWidth,
            Margin = new Thickness(0, 0, 16, 16), Child = row
         };

         button.Click += delegate {
            string path = pickFile(title, filter);
            if (path == null) return;
            chosen(path);
            setCurrent(path);
            status.Text = describeSlotFile(path);
            thumbnail.Source = ImageFile.Load(path);
            clearButton.IsEnabled = true;
         };
         clearButton.Click += delegate {
            if (cleared != null) cleared();
            setCurrent("");
            status.Text = "none";
            thumbnail.Source = null;
            clearButton.IsEnabled = false;
         };
         panel.Children.Add(card);

         // reopening a project must put the card back in step with what it holds
         slotRefreshers.Add(delegate {
            held = readCurrent();
            status.Text = describeSlotFile(held);
            thumbnail.Source = ImageFile.Load(project.ResolveAsset(held));
            clearButton.IsEnabled = !string.IsNullOrEmpty(held);
         });
      }

      // a section is a heading with its cards wrapping into as many columns as the window allows
      // "beep.wav (wav)" rather than just a filename: a wav is converted when the theme is built,
      // a vag is used as it is, and which one you have changes what happens
      private static string describeSlotFile(string path)
      {
         if (string.IsNullOrEmpty(path)) return "none";
         string extension = Path.GetExtension(path).TrimStart('.').ToLowerInvariant();
         bool isSound = extension == "wav" || extension == "vag";
         return Path.GetFileName(path) + (isSound ? "   (" + extension + ")" : "");
      }

      // plays a wav so a sound can be judged before it goes in; a vag is the console's own format
      // and nothing on the pc can play it
      private void playSound(string path)
      {
         string full = project.ResolveAsset(path);
         if (full.Length == 0 || !File.Exists(full)) { log("no sound set to play"); return; }
         if (!SoundConvert.IsWav(full)) {
            log(Path.GetFileName(full) + " is a console sound, which nothing on this computer can play");
            return;
         }
         try {
            var player = new System.Media.SoundPlayer(full);
            player.Play();
         } catch (Exception exception) {
            log("could not play " + Path.GetFileName(full) + ": " + exception.Message);
         }
      }

      private WrapPanel addSection(Panel panel, string text, string hint)
      {
         panel.Children.Add(new TextBlock {
            Text = text, FontSize = 13, FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, panel.Children.Count == 0 ? 0 : 32, 0, 4)
         });
         panel.Children.Add(new TextBlock {
            Text = hint, Foreground = DimBrush, FontSize = 11, TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 12)
         });
         var cards = new WrapPanel();
         panel.Children.Add(cards);
         return cards;
      }

      // icon grid

      private void buildIconGrid()
      {
         addSection(iconPanel, "Menu icons",
                    "Click a slot to choose a picture, or the cross to clear it. Anything left alone keeps " +
                    "the console's own icon. 128x128, except the photo and video ones. To set many at once, " +
                    "name your pictures after the slots (icon_game.png, icon_music.png ...) and import them " +
                    "together -- the program's own assets\\default-icons folder is a ready-made naming guide.");

         var importButton = new Button { Content = "Import a set of icons", HorizontalAlignment = HorizontalAlignment.Left };
         importButton.Click += onImportIcons;
         iconPanel.Children.Add(importButton);

         foreach (string group in IconSlots.Groups) {
            iconPanel.Children.Add(new TextBlock {
               Text = group, FontSize = 13, FontWeight = FontWeights.SemiBold,
               Margin = new Thickness(0, 24, 0, 12)
            });
            var wrap = new WrapPanel();
            foreach (IconSlot slot in IconSlots.All)
               if (slot.Group == group) wrap.Children.Add(makeIconTile(slot));
            iconPanel.Children.Add(wrap);
         }
      }

      private Button makeIconTile(IconSlot slot)
      {
         var thumbnail = new Image { Width = 44, Height = 44, Stretch = Stretch.Uniform };
         iconThumbnails[slot.Id] = thumbnail;

         var frame = new Border {
            Width = 56, Height = 56, CornerRadius = new CornerRadius(4), Background = SurfaceBrush,
            BorderBrush = EdgeBrush, BorderThickness = new Thickness(1), Child = thumbnail
         };
         var caption = new TextBlock {
            Text = slot.Label, FontSize = 11, TextWrapping = TextWrapping.Wrap, Width = 96,
            TextAlignment = TextAlignment.Center, Foreground = DimBrush, Margin = new Thickness(0, 8, 0, 0)
         };
         // sits on the corner of the picture, so it has to stand out against whatever is behind it
         var clearCross = new Button {
            Content = "✕", Width = 18, Height = 18, Padding = new Thickness(0),
            Margin = new Thickness(0, -6, -6, 0), FontSize = 10, Foreground = Brushes.White,
            Background = ClearCrossBrush, BorderBrush = EdgeBrush,
            HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top,
            Tag = slot, Visibility = Visibility.Collapsed
         };
         AutomationProperties.SetName(clearCross, "Clear " + slot.Label + " icon");
         clearCross.Click += onClearIconSlot;
         iconClearButtons[slot.Id] = clearCross;

         var framed = new Grid();
         framed.Children.Add(frame);
         framed.Children.Add(clearCross);

         var content = new StackPanel { HorizontalAlignment = HorizontalAlignment.Center };
         content.Children.Add(framed);
         content.Children.Add(caption);

         var tile = new Button {
            Content = content, Tag = slot, Width = 112, Height = 108, Padding = new Thickness(4),
            Margin = new Thickness(0, 0, 8, 8), Background = Brushes.Transparent,
            BorderBrush = Brushes.Transparent, Template = (ControlTemplate)FindResource("PlainTile"),
            ToolTip = slot.Id + "  (" + slot.Width + "x" + slot.Height + ")"
         };
         // only the picture lights up, not the whole tile: a tile-wide highlight would paint over
         // the clear cross sitting on its corner
         tile.MouseEnter += delegate { frame.Background = HoverBrush; };
         tile.MouseLeave += delegate { frame.Background = SurfaceBrush; };
         // the tile's content is a picture and a caption, so it needs to be told its own name
         AutomationProperties.SetName(tile, slot.Label);
         tile.Click += onPickIconSlot;
         return tile;
      }

      private void onPickIconSlot(object sender, RoutedEventArgs e)
      {
         var slot = (IconSlot)((Button)sender).Tag;
         string path = pickFile(slot.Label + " icon", "png images (*.png)|*.png");
         if (path == null) return;
         project.IconPaths[slot.Id] = path;
         showIconSlot(slot.Id);
         warnUnlessSize(path, slot.Width, slot.Height, slot.Label + " icon");
         refreshPreview();
      }

      // a whole set of icons in one go, matched to slots by filename. anything that does not name
      // a slot is left alone and said so, rather than guessed at.
      private void onImportIcons(object sender, RoutedEventArgs e)
      {
         var dialog = new OpenFileDialog {
            Title = "Choose the icons to import (Ctrl+A takes the whole folder)",
            Filter = "png images (*.png)|*.png", Multiselect = true
         };
         if (dialog.ShowDialog() != true) return;

         var skipped = new List<string>();
         var wrongSize = new List<string>();
         int imported = 0;

         foreach (string path in dialog.FileNames) {
            IconSlot slot = findSlotByFileName(path);
            if (slot == null) { skipped.Add(Path.GetFileName(path)); continue; }

            project.IconPaths[slot.Id] = path;
            showIconSlot(slot.Id);
            imported++;

            int width, height;
            if (ImageFile.TryReadSize(path, out width, out height) &&
                (width != slot.Width || height != slot.Height))
               wrongSize.Add(Path.GetFileName(path) + " is " + width + "x" + height);
         }

         log("imported " + imported + (imported == 1 ? " icon" : " icons"));
         if (skipped.Count > 0)
            log("   no slot goes by these names, so they were left out: " + string.Join(", ", skipped.ToArray()));
         if (wrongSize.Count > 0)
            log("   wrong size, so they will build but may look wrong: " + string.Join(", ", wrongSize.ToArray()));
         refreshPreview();
      }

      // "icon_game.png", or "game.png" for anyone who dropped the prefix
      private static IconSlot findSlotByFileName(string path)
      {
         string name = Path.GetFileNameWithoutExtension(path).ToLowerInvariant();
         foreach (IconSlot slot in IconSlots.All)
            if (slot.Id == name || slot.Id == "icon_" + name) return slot;
         return null;
      }

      private void onClearIconSlot(object sender, RoutedEventArgs e)
      {
         var slot = (IconSlot)((Button)sender).Tag;
         e.Handled = true;   // the cross sits on the tile, and must not also count as choosing
         if (!project.IconPaths.Remove(slot.Id)) return;
         showIconSlot(slot.Id);
         log("cleared " + slot.Label + " icon");
         refreshPreview();
      }

      private void showIconSlot(string id)
      {
         Image thumbnail;
         if (!iconThumbnails.TryGetValue(id, out thumbnail)) return;
         string storedPath;
         thumbnail.Source = project.IconPaths.TryGetValue(id, out storedPath)
            ? ImageFile.Load(project.ResolveAsset(storedPath))
            : null;

         Button clearCross;
         if (iconClearButtons.TryGetValue(id, out clearCross))
            clearCross.Visibility = thumbnail.Source == null ? Visibility.Collapsed : Visibility.Visible;
      }

      // actions

      private void onNew(object sender, RoutedEventArgs e)
      {
         replaceProject(new ThemeProject());
         log("new project");
         goToDetails();
      }

      // a project is unpacked into a temporary folder while it is open, so the one being put
      // aside has to let go of its own
      private void replaceProject(ThemeProject opened)
      {
         showScenePlaying(false);
         if (project != null) project.Close();
         project = opened;
         showProject();
      }

      protected override void OnClosed(EventArgs e)
      {
         showScenePlaying(false);
         if (project != null) project.Close();
         base.OnClosed(e);
      }

      // once there is a project to work on, the Start stage has nothing left to offer
      private void goToDetails() { tabs.SelectedIndex = 1; }

      private void onOpen(object sender, RoutedEventArgs e)
      {
         var dialog = new OpenFileDialog { Filter = "theme project (*.themeproj)|*.themeproj" };
         if (dialog.ShowDialog() != true) return;
         try {
            replaceProject(ThemeProject.Load(dialog.FileName));
            log("opened " + dialog.FileName);
            goToDetails();
         } catch (Exception exception) {
            log("could not open: " + exception.Message);
         }
      }

      private void onSave(object sender, RoutedEventArgs e)
      {
         readProject();

         // a file that has gone would be saved as a reference to nothing, and the copy inside the
         // project is usually the only one left. saving to a new file is still allowed, so this
         // cannot trap someone's work in the window.
         List<string> missing = project.FindMissingAssets();
         if (missing.Count > 0) {
            log(missing.Count + (missing.Count == 1 ? " file this project uses is" : " files this project uses are") +
                " missing:");
            foreach (string entry in missing) log("   " + entry);
            if (project.ProjectPath.Length > 0) {
               log("not saved, so " + Path.GetFileName(project.ProjectPath) + " keeps what it still holds -- " +
                   "put the files back, or remove the objects that use them");
               return;
            }
         }

         if (project.ProjectPath.Length == 0) {
            var dialog = new SaveFileDialog { Filter = "theme project (*.themeproj)|*.themeproj",
                                              FileName = project.Name + ".themeproj" };
            if (dialog.ShowDialog() != true) return;
            project.ProjectPath = dialog.FileName;
         }
         project.Save(project.ProjectPath);
         showProjectName();   // a first save gives the project its name, so the title picks it up
         log("saved " + project.ProjectPath + " -- pictures, models, sounds and the script are all inside it");
      }

      // the label at the bottom of Start and the window title, both naming the open project
      private void showProjectName()
      {
         bool saved = project.ProjectPath.Length > 0;
         projectLabel.Text = saved ? "Working on " + project.ProjectPath : "No project saved yet.";
         Title = saved ? "Theme Studio - " + Path.GetFileName(project.ProjectPath) : "Theme Studio";
      }


      // a small menu of starting points, rather than one example that may not be the one wanted
      private void onShowExamples(object sender, RoutedEventArgs e)
      {
         // opens off the button it belongs to, rather than wherever the mouse happens to be.
         // upwards, because the button sits at the bottom of the stage.
         var menu = new ContextMenu {
            PlacementTarget = exampleButton, Placement = PlacementMode.Top, IsOpen = true
         };
         foreach (PsjsExample example in PsjsExamples.All) {
            PsjsExample chosen = example;
            var entry = new MenuItem { Header = chosen.Title, InputGestureText = chosen.Summary };
            entry.Click += delegate { insertExample(chosen); };
            menu.Items.Add(entry);
         }
      }

      private void onAddPrimary(object sender, RoutedEventArgs e)
      {
         if (sceneToggle.IsChecked == true) onAddSceneModel(sender, e);
         else addBackgroundPicture();
      }

      private void onRemoveSelected(object sender, RoutedEventArgs e)
      {
         if (sceneToggle.IsChecked == true) onRemoveActor(sender, e);
         else removeBackground();
      }

      private void addBackgroundPicture()
      {
         if (isBackgroundListFull()) return;
         string path = pickFile("background image", "jpeg images (*.jpg;*.jpeg)|*.jpg;*.jpeg");
         if (path == null) return;
         project.Backgrounds.Add(new Background { WidescreenPath = path });
         showBackgrounds();
         warnUnlessSize(path, 1920, 1080, "widescreen background");
         refreshPreview();
      }

      private void removeBackground()
      {
         int index = backgroundList.SelectedIndex;
         if (index < 0) return;
         project.Backgrounds.RemoveAt(index);
         showBackgrounds();
         refreshPreview();
      }

      private void onBuild(object sender, RoutedEventArgs e) { build(null); }

      // deploying always builds first: there is no way to know whether what is on disk still
      // matches the project, and a build is cheap next to shipping the wrong theme
      private void onDeploy(object sender, RoutedEventArgs e) { build(push); }

      // build runs off the ui thread so the window stays responsive while the compiler works.
      // only one at a time: two builds share a staging folder and would delete each other's files
      // half-written, and the second would read a project the first is still walking.
      private bool building;

      // afterwards runs on the build thread with the finished .p3t, or null to just build
      private void build(Action<string> afterwards)
      {
         if (building) { log("a build is already running"); return; }
         readProject();
         if (project.Backgrounds.Count == 0) {
            log("nothing to build: add at least one background");
            return;
         }
         showBuilding(true);
         ThemeProject beingBuilt = project;
         ThreadPool.QueueUserWorkItem(delegate {
            try {
               ThemeBuild.BuildResult result = ThemeBuild.Build(beingBuilt, log);
               if (result.Succeeded && afterwards != null) afterwards(result.OutputPath);
            } catch (Exception exception) {
               log("build failed: " + exception.Message);
            } finally {
               Dispatcher.BeginInvoke(new Action(delegate { showBuilding(false); }));
            }
         });
      }

      private void showBuilding(bool running)
      {
         building = running;
         buildButton.IsEnabled = !running;
         deployButton.IsEnabled = !running;
      }

      private void onPs3AddressChanged(object sender, RoutedEventArgs e)
      {
         string wanted = ps3AddressBox.Text.Trim();
         if (wanted == AppSettings.Ps3Ip) return;
         AppSettings.Set("ps3ip", wanted);
         log(wanted.Length == 0 ? "PS3 address cleared" : "PS3 address set to " + wanted);
      }

      private void push(string p3tPath)
      {
         string ip = AppSettings.Ps3Ip;
         if (ip.Length == 0) {
            log("no PS3 address set -- type your console's address on this page first");
            return;
         }

         List<string> onConsole;
         string error;
         if (!Ps3Deploy.TryListThemes(ip, out onConsole, out error)) {
            log("cannot reach " + ip + ": " + error);
            log("check the console is on and running an FTP server (e.g. simple-ftp)");
            return;
         }

         // replacing a theme already there costs no slot, so only a genuinely new one is stopped
         string fileName = Path.GetFileName(p3tPath);
         if (!Ps3Deploy.HoldsTheme(onConsole, fileName) && onConsole.Count >= Ps3Deploy.MaxThemes) {
            log("not sent: the console is already holding " + onConsole.Count + " themes, and it lists at " +
                "most " + Ps3Deploy.MaxThemes + " -- delete one on the console and try again");
            return;
         }

         log("sending to " + ip + Ps3Deploy.ThemeDir + "...");
         Ps3Deploy.Upload(ip, p3tPath);
         log("done -- it should now be listed under Settings > Theme Settings");
      }

      // helpers

      private bool isBackgroundListFull()
      {
         if (project.Backgrounds.Count < ThemeProject.MaxBackgrounds) return false;
         log("a theme can hold at most " + ThemeProject.MaxBackgrounds + " backgrounds");
         return true;
      }

      // the compilers accept wrongly sized images without complaint and the console then renders
      // them badly, so the editor is the only place this can be caught.
      private void warnUnlessSize(string path, int expectedWidth, int expectedHeight, string what)
      {
         int width, height;
         if (!ImageFile.TryReadSize(path, out width, out height)) {
            log("added " + what + " (could not read its size)");
            return;
         }
         if (width == expectedWidth && height == expectedHeight) {
            log("added " + what + " (" + width + "x" + height + ")");
            return;
         }
         log("warning: " + what + " is " + width + "x" + height + ", expected " +
             expectedWidth + "x" + expectedHeight + " -- it will build but may look wrong");
      }

      private string pickFile(string title, string filter)
      {
         var dialog = new OpenFileDialog { Title = title, Filter = filter };
         return dialog.ShowDialog() == true ? dialog.FileName : null;
      }

      private static int clamp(int value, int low, int high)
      {
         return value < low ? low : (value > high ? high : value);
      }

      private void log(string message)
      {
         if (!Dispatcher.CheckAccess()) { Dispatcher.BeginInvoke(new Action<string>(log), message); return; }
         logBox.AppendText(message + Environment.NewLine);
         logBox.ScrollToEnd();
      }
   }
}
