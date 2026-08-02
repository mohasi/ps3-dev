using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;

namespace PatchStudio
{
   public partial class MainWindow : Window
   {
      private DumpProject project;

      private FileSystemWatcher editWatcher;
      private readonly DispatcherTimer editDebounce = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
      private readonly HashSet<string> pendingHashes = new HashSet<string>();

      // set whenever an edit lands (or a fresh dump arrives), cleared by a build. Deploy only rebuilds
      // when this is set, so an unchanged patch redeploys its existing build/ untouched.
      private bool buildDirty;

      // set whenever the scratch folder gains work not yet written to the .patchproj (a downloaded dump
      // or a saved edit), cleared by Save. drives the "save first?" prompt on New / Open / window-close,
      // so the user is never silently dropped back to nothing.
      private bool dirty;

      public MainWindow()
      {
         InitializeComponent();
         ProjectPackage.SweepLeftovers();   // clear temp folders left by a past crash
         // a save in an external editor fires the watcher; debounce so a multi-write save re-checks once
         editDebounce.Tick += (s, e) => { editDebounce.Stop(); FlushPending(); };
         Closing += (s, e) => { if (!CloseCurrent()) e.Cancel = true; };
      }

      // a new project needs only a name and the title ID it patches. it lives in a scratch folder,
      // unsaved, until Save writes it to a .patchproj.
      private void NewProject_Click(object sender, RoutedEventArgs e)
      {
         var dialog = new NewProjectDialog("my patch") { Owner = this };
         if (dialog.ShowDialog() != true) return;

         if (!CloseCurrent()) return;
         try { ShowProject(DumpProject.New(dialog.ProjectName, dialog.GameId)); }
         catch (Exception ex) { MessageBox.Show(this, "Couldn't create the project:\n" + ex.Message, "New Project"); }
      }

      private void OpenProject_Click(object sender, RoutedEventArgs e)
      {
         var dialog = new OpenFileDialog { Title = "Open project", Filter = "Patch project (*.patchproj)|*.patchproj" };
         if (dialog.ShowDialog(this) != true) return;
         OpenPackage(dialog.FileName);
      }

      public void OpenPackage(string packagePath)
      {
         if (!CloseCurrent()) return;
         try { ShowProject(DumpProject.Open(packagePath)); }
         catch (Exception ex) { MessageBox.Show(this, "Couldn't open the project:\n" + ex.Message, "Patch Studio"); }
      }

      private void Save_Click(object sender, RoutedEventArgs e) { SaveProject(); }

      // save to the project's .patchproj, asking for a location the first time. returns false if the
      // user cancelled the location prompt or the write failed (so a save-then-close can abort).
      private bool SaveProject()
      {
         if (project == null) return true;

         // an auto-created project (from Download Dump) has only a placeholder name — name it first, with
         // its title id pre-filled, so the deploy path patches/<gameId>/<name>/ is what the user intends.
         if (project.PackagePath == "" && project.NeedsNaming)
         {
            var nameDialog = new NewProjectDialog(project.Name, project.GameId) { Owner = this };
            if (nameDialog.ShowDialog() != true) return false;
            project.Name = nameDialog.ProjectName;
            project.GameId = nameDialog.GameId;
            project.NeedsNaming = false;
         }

         string path = project.PackagePath;
         if (path == "")
         {
            var dialog = new SaveFileDialog { Filter = "Patch project (*.patchproj)|*.patchproj", FileName = project.Name + ".patchproj" };
            if (dialog.ShowDialog(this) != true) return false;
            path = dialog.FileName;
         }
         try { project.Save(path); }
         catch (Exception ex) { MessageBox.Show(this, "Couldn't save:\n" + ex.Message, "Save"); return false; }
         dirty = false;
         ShowTitle();
         statusText.Text = "saved  ·  " + ProjectStatus();
         return true;
      }

      // close the open project's scratch folder before opening/creating another (or quitting). if it has
      // unsaved work, offer to save first; Cancel aborts the whole operation.
      private bool CloseCurrent()
      {
         if (project == null) return true;
         if (dirty)
         {
            var choice = MessageBox.Show(this, "Save changes to \"" + project.Name + "\" first?", "Patch Studio",
                                         MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
            if (choice == MessageBoxResult.Cancel) return false;
            if (choice == MessageBoxResult.Yes && !SaveProject()) return false;   // save cancelled/failed: don't discard
         }
         project.Close();
         project = null;
         return true;
      }

      private void ShowProject(DumpProject opened)
      {
         project = opened;
         RefreshGallery();
         RefreshEditedFlags();            // badge anything already edited
         WatchEdits(project.Folder);      // then react live to future saves
         buildDirty = false;
         dirty = false;
         ShowTitle();
         statusText.Text = ProjectStatus();
      }

      // point the gallery at the project's textures. clearing first forces a repaint even when the
      // list object is the same one (a plain List gives the ListBox no change notification).
      private void RefreshGallery()
      {
         gallery.ItemsSource = null;
         gallery.ItemsSource = project.Textures;
      }

      private void ShowTitle()
      {
         Title = project == null ? "Patch Studio"
                                 : "Patch Studio — " + project.Name + (project.IsSaved ? "" : " *");
      }

      private string ProjectStatus()
      {
         string game = project.GameId == "" ? "no title ID" : project.GameId;
         string where = project.IsSaved ? project.PackagePath : "unsaved";
         return project.Textures.Count + " textures  ·  " + game + "  ·  " + where;
      }

      // the console dump root; the plugin dumps each game to a <titleId> subfolder (manifest.txt + <hash>.bin),
      // so dumps for different games don't collide and survive game restarts.
      private const string RemoteDumpsDir = "/dev_hdd0/tmp/simple-cheat-menu/dumps";

      // list the per-game dump folders on the console, let the user pick one, then pull it. if no project
      // is open, one is auto-created for the picked title id.
      private void DownloadDump_Click(object sender, RoutedEventArgs e)
      {
         string ip = AppSettings.Ps3Ip;
         if (string.IsNullOrEmpty(ip)) { MessageBox.Show(this, "Set ps3ip in settings.txt first.", "Fetch Dump"); return; }

         SetBusy(true, "Listing dumps on " + ip + "…");
         System.Threading.Tasks.Task.Factory.StartNew(() =>
         {
            List<string> titleIds; string error;
            bool ok = Ps3Ftp.TryListFiles(ip, RemoteDumpsDir, out titleIds, out error);
            Dispatcher.Invoke(new Action(() =>
            {
               SetBusy(false, null);
               if (!ok) { MessageBox.Show(this, "Couldn't list dumps:\n" + error, "Fetch Dump"); return; }
               if (titleIds.Count == 0) { MessageBox.Show(this, "No dumps on the console yet — play a game and press Square to dump its textures first.", "Fetch Dump"); return; }

               titleIds.Sort();
               var picker = new SelectDumpDialog(titleIds, project != null ? project.GameId : null) { Owner = this };
               if (picker.ShowDialog() != true) return;
               PullDump(ip, picker.Selected);
            }));
         });
      }

      // pull the chosen game's dump into the open project (auto-creating one for that title id if none is open),
      // then reload: new textures merge, the edited/ folder is left untouched.
      private void PullDump(string ip, string titleId)
      {
         if (project == null)
         {
            try
            {
               var created = DumpProject.New("untitled", titleId);
               created.NeedsNaming = true;   // placeholder name: the first Save will prompt for a real one
               ShowProject(created);
            }
            catch (Exception ex) { MessageBox.Show(this, "Couldn't create a project:\n" + ex.Message, "Fetch Dump"); return; }
         }

         string remoteDir = RemoteDumpsDir + "/" + Escape(titleId);
         string target = project.Folder;
         SetBusy(true, "Downloading " + titleId + " dump from " + ip + "…");
         System.Threading.Tasks.Task.Factory.StartNew(() =>
         {
            string error;
            int pulled = DownloadDumpFiles(ip, remoteDir, target, out error);
            Dispatcher.Invoke(new Action(() =>
            {
               SetBusy(false, null);
               if (error != null) { MessageBox.Show(this, "Fetch failed:\n" + error, "Fetch Dump"); return; }
               project.Load();
               RefreshGallery();   // Load refills the same list, which the ListBox won't notice on its own
               RefreshEditedFlags();
               buildDirty = true;
               dirty = true;
               statusText.Text = pulled + " file(s) downloaded  ·  " + ProjectStatus();
            }));
         });
      }

      // returns files pulled; error stays null on success
      private static int DownloadDumpFiles(string ip, string remoteDir, string target, out string error)
      {
         List<string> files;
         if (!Ps3Ftp.TryListFiles(ip, remoteDir, out files, out error)) return 0;
         if (files.Count == 0) { error = "That dump folder is empty."; return 0; }

         Directory.CreateDirectory(target);
         int pulled = 0;
         foreach (string name in files)
         {
            try { Ps3Ftp.DownloadFile(ip, remoteDir + "/" + name, Path.Combine(target, name)); pulled++; }
            catch (Exception ex) { error = "Failed on " + name + ":\n" + ex.Message; return pulled; }
         }
         error = null;
         return pulled;
      }

      // show the spinner overlay over the window (it blocks input by covering everything) while an FTP
      // transfer runs; hide it when done. the spin runs only while shown.
      private void SetBusy(bool busy, string status)
      {
         busyOverlay.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
         if (busy)
         {
            if (status != null) busyText.Text = status;
            var spin = new System.Windows.Media.Animation.DoubleAnimation(0, 360, TimeSpan.FromSeconds(0.9))
                       { RepeatBehavior = System.Windows.Media.Animation.RepeatBehavior.Forever };
            spinnerRotation.BeginAnimation(RotateTransform.AngleProperty, spin);
         }
         else
         {
            spinnerRotation.BeginAnimation(RotateTransform.AngleProperty, null);   // stop animating
            if (status != null) statusText.Text = status;
         }
      }

      // build a patch (only the edited textures) into a single .patch file the user chooses and Deploy remembers.
      private void BuildPatch_Click(object sender, RoutedEventArgs e)
      {
         if (project == null) { MessageBox.Show(this, "Open a project first.", "Build"); return; }

         string path = PickBuildFile();
         if (path == null) return;
         project.BuildPath = path;
         dirty = true;   // the remembered build location is project state, so it should be saved

         PatchBuilder.Result result;
         try { result = RunBuild(); }
         catch (Exception ex) { MessageBox.Show(this, "Build failed:\n" + ex.Message, "Build"); return; }

         if (result.Included == 0 && result.Skipped.Count == 0)
         {
            MessageBox.Show(this, "No edited textures to build. Double-click a texture to edit it, change it, and save first.", "Build");
            return;
         }
         string message = result.Included + " texture(s) written to:\n" + project.BuildPath;
         if (result.Skipped.Count > 0) message += "\n\nSkipped:\n  " + string.Join("\n  ", result.Skipped.ToArray());
         MessageBox.Show(this, message, "Build");
      }

      // encode the edited textures into a scratch folder, then pack them into the remembered .patch file.
      private PatchBuilder.Result RunBuild()
      {
         string scratch = ProjectPackage.MakeWorkDir();
         try
         {
            PatchBuilder.Result result = PatchBuilder.Build(project, scratch);
            ProjectPackage.Pack(scratch, project.BuildPath);
            buildDirty = false;
            return result;
         }
         finally { ProjectPackage.Discard(scratch); }
      }

      // ask for the .patch file to build, defaulting the name to the patch name and the folder to the
      // last-used one. null if cancelled.
      private string PickBuildFile()
      {
         var dialog = new SaveFileDialog
         {
            Title = "Build patch to",
            Filter = "Patch (*.patch)|*.patch",
            FileName = (project.Name == "" ? "patch" : project.Name) + ".patch"
         };
         if (project.BuildPath != "")
         {
            string dir = Path.GetDirectoryName(project.BuildPath);
            if (dir != null && Directory.Exists(dir)) dialog.InitialDirectory = dir;
            dialog.FileName = Path.GetFileName(project.BuildPath);
         }
         return dialog.ShowDialog(this) == true ? dialog.FileName : null;
      }

      // the files that make up a built patch in dir: manifest.txt plus exactly the .bin files it lists (so
      // Deploy never uploads stale or unrelated files that happen to sit in a user-chosen folder).
      private static string[] PatchFiles(string dir)
      {
         string manifest = Path.Combine(dir, "manifest.txt");
         if (!File.Exists(manifest)) return new string[0];
         var files = new List<string> { manifest };
         foreach (string raw in File.ReadAllLines(manifest))
         {
            string line = raw.Trim();
            if (line == "" || line.StartsWith("#")) continue;
            string[] token = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (token.Length < 6) continue;
            string bin = Path.Combine(dir, token[5]);
            if (File.Exists(bin)) files.Add(bin);
         }
         return files.ToArray();
      }

      // the console root the plugin's Patches tab reads; each patch lives at <root>/<gameId>/<name>/
      private const string RemotePatchesDir = "/dev_hdd0/tmp/simple-cheat-menu/patches";

      // build if anything changed since the last build, then upload the .patch's files to patches/<gameId>/<name>/.
      private void Deploy_Click(object sender, RoutedEventArgs e)
      {
         if (project == null) { MessageBox.Show(this, "Open a project first.", "Deploy"); return; }
         if (project.GameId == "") { MessageBox.Show(this, "This project has no title ID. Set it in New Project (or project.txt).", "Deploy"); return; }
         string ip = AppSettings.Ps3Ip;
         if (string.IsNullOrEmpty(ip)) { MessageBox.Show(this, "Set ps3ip in settings.txt first.", "Deploy"); return; }

         // deploy from the remembered .patch file; if nothing was ever built, ask where to build it now
         if (project.BuildPath == "")
         {
            string path = PickBuildFile();
            if (path == null) return;
            project.BuildPath = path;
            dirty = true;
         }

         if (buildDirty || !File.Exists(project.BuildPath))
         {
            try { RunBuild(); }
            catch (Exception ex) { MessageBox.Show(this, "Build failed:\n" + ex.Message, "Deploy"); return; }
         }

         // unpack the .patch to a temp folder to read its files; discarded after the upload finishes
         string temp;
         try { temp = ProjectPackage.Unpack(project.BuildPath); }
         catch (Exception ex) { MessageBox.Show(this, "Couldn't read the built patch:\n" + ex.Message, "Deploy"); return; }

         string[] files = PatchFiles(temp);
         if (files.Length <= 1) { ProjectPackage.Discard(temp); MessageBox.Show(this, "Nothing to deploy — edit a texture first.", "Deploy"); return; }   // <= 1 = manifest only, no textures

         string remoteDir = RemotePatchesDir + "/" + Escape(project.GameId) + "/" + Escape(project.Name);
         SetBusy(true, "Deploying to " + ip + "…");
         System.Threading.Tasks.Task.Factory.StartNew(() =>
         {
            string error = DeployFiles(ip, remoteDir, files);
            Dispatcher.Invoke(new Action(() =>
            {
               ProjectPackage.Discard(temp);
               SetBusy(false, null);
               statusText.Text = ProjectStatus();
               if (error != null) { MessageBox.Show(this, "Deploy failed:\n" + error, "Deploy"); return; }
               MessageBox.Show(this, (files.Length - 1) + " texture(s) deployed to:\n" + RemotePatchesDir + "/" + project.GameId + "/" + project.Name, "Deploy");
            }));
         });
      }

      private static string DeployFiles(string ip, string remoteDir, string[] files)
      {
         try
         {
            // FTP mkdir isn't recursive, so make each level in turn
            Ps3Ftp.MakeDir(ip, RemotePatchesDir);
            Ps3Ftp.MakeDir(ip, remoteDir.Substring(0, remoteDir.LastIndexOf('/')));   // patches/<gameId>
            Ps3Ftp.MakeDir(ip, remoteDir);                                            // patches/<gameId>/<name>
            foreach (string file in files)
               Ps3Ftp.UploadFile(ip, remoteDir + "/" + Escape(Path.GetFileName(file)), File.ReadAllBytes(file));
            return null;
         }
         catch (Exception ex) { return ex.Message; }
      }

      // encode one path segment so spaces and the like survive the FTP URI (the console sees the real name)
      private static string Escape(string segment) { return Uri.EscapeDataString(segment); }

      // double-left-click a tile opens it in the Windows default image viewer (MouseDoubleClick is
      // left-button only, and the first click of the pair selects the tile).
      private void Gallery_DoubleClick(object sender, MouseButtonEventArgs e)
      {
         View(gallery.SelectedItem as TextureItem);
      }

      // right-click selects the tile under the cursor so the context menu acts on it (mirrors rco-studio).
      private void Gallery_RightButtonDown(object sender, MouseButtonEventArgs e)
      {
         var element = e.OriginalSource as DependencyObject;
         while (element != null && !(element is ListBoxItem)) element = VisualTreeHelper.GetParent(element);
         if (element is ListBoxItem) ((ListBoxItem)element).IsSelected = true;
      }

      // enable Edit only when the configured image editor actually exists; otherwise disable it and point
      // the user at the setting via a tooltip (shown on the disabled item via ToolTipService.ShowOnDisabled).
      private void TileMenu_Opened(object sender, RoutedEventArgs e)
      {
         bool available = EditorAvailable;
         editMenuItem.IsEnabled = available;
         editMenuItem.ToolTip = available ? null : "Set imageEditor in settings.txt to an installed image editor to enable editing.";
      }

      private void EditMenu_Click(object sender, RoutedEventArgs e)
      {
         Edit(gallery.SelectedItem as TextureItem);
      }

      private static bool EditorAvailable { get { return !string.IsNullOrEmpty(AppSettings.ImageEditor) && File.Exists(AppSettings.ImageEditor); } }

      // open the texture as a PNG in the Windows default viewer
      private void View(TextureItem item)
      {
         if (item == null || item.Thumbnail == null) return;
         string png = Path.Combine(Path.GetTempPath(), "patch-studio-view-" + item.Hash + ".png");
         SavePng((BitmapSource)item.Thumbnail, png);
         Launch(null, png);
      }

      // export an editable PNG next to the dump and open it in the configured editor. we do NOT mark
      // it edited here — that only happens once the saved pixels actually differ (see RefreshEditedFlags).
      private void Edit(TextureItem item)
      {
         if (item == null || item.Original == null || project == null) return;
         string editDir = Path.Combine(project.Folder, "edited");
         Directory.CreateDirectory(editDir);
         string png = Path.Combine(editDir, item.Hash + ".png");
         if (!File.Exists(png)) SavePng((BitmapSource)item.Original, png);   // start the edit from the original texture
         item.EditPath = png;
         Launch(AppSettings.ImageEditor, png);
      }

      // watch the edited/ folder so a save in an external editor re-checks just that texture, live —
      // no full re-scan on focus. batched through a debounce timer since one save can fire several events.
      private void WatchEdits(string folder)
      {
         if (editWatcher != null) { editWatcher.Dispose(); editWatcher = null; }
         string editDir = Path.Combine(folder, "edited");
         Directory.CreateDirectory(editDir);
         editWatcher = new FileSystemWatcher(editDir, "*.png")
         {
            NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size,
            EnableRaisingEvents = true
         };
         FileSystemEventHandler onChange = (s, e) => QueueRecheck(Path.GetFileNameWithoutExtension(e.Name));
         editWatcher.Changed += onChange;
         editWatcher.Created += onChange;
         editWatcher.Renamed += (s, e) => QueueRecheck(Path.GetFileNameWithoutExtension(e.Name));
      }

      // watcher events arrive on a background thread; hop to the UI thread and coalesce via the timer
      private void QueueRecheck(string hash)
      {
         Dispatcher.BeginInvoke(new Action(() =>
         {
            pendingHashes.Add(hash);
            editDebounce.Stop(); editDebounce.Start();
         }));
      }

      private void FlushPending()
      {
         foreach (string hash in pendingHashes)
            foreach (TextureItem item in project.Textures)
               if (item.Hash == hash) { Recheck(item); break; }
         pendingHashes.Clear();
         buildDirty = true;   // an edit was saved, so the next deploy rebuilds
         dirty = true;        // and there's now unsaved work in the scratch folder
      }

      // full pass when a project opens: badge anything already edited from a previous session
      private void RefreshEditedFlags()
      {
         if (project == null) return;
         foreach (TextureItem item in project.Textures) Recheck(item);
      }

      // an edited/<hash>.png whose pixels differ from the original decode is a real change (comparing
      // pixels, not bytes, ignores PNG re-encode noise). when it is a change, show the edited image in the
      // gallery in place of the original; otherwise show the original.
      private void Recheck(TextureItem item)
      {
         string png = Path.Combine(project.Folder, "edited", item.Hash + ".png");
         BitmapSource edited = File.Exists(png) ? LoadPixels(png) : null;
         bool changed = edited != null && PixelsDiffer((BitmapSource)item.Original, edited);
         item.Edited = changed;
         item.Thumbnail = changed ? edited : item.Original;
      }

      private static BitmapSource LoadPixels(string path)
      {
         try
         {
            var image = new BitmapImage();
            image.BeginInit();
            image.CacheOption = BitmapCacheOption.OnLoad;   // don't lock the file, so the editor can re-save
            image.CreateOptions = BitmapCreateOptions.IgnoreImageCache;   // re-read the file each time, or a re-saved edit shows the stale first decode
            image.UriSource = new Uri(path);
            image.EndInit();
            image.Freeze();
            return image;
         }
         catch { return null; }
      }

      private static bool PixelsDiffer(BitmapSource original, BitmapSource edited)
      {
         if (original == null || edited == null) return false;
         if (edited.PixelWidth != original.PixelWidth || edited.PixelHeight != original.PixelHeight) return true;
         byte[] a = ToBgra(original), b = ToBgra(edited);
         if (a.Length != b.Length) return true;
         for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return true;
         return false;
      }

      private static byte[] ToBgra(BitmapSource source)
      {
         var converted = new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0);
         int stride = converted.PixelWidth * 4;
         byte[] pixels = new byte[stride * converted.PixelHeight];
         converted.CopyPixels(pixels, stride, 0);
         return pixels;
      }

      private static void SavePng(BitmapSource image, string path)
      {
         var encoder = new PngBitmapEncoder();
         encoder.Frames.Add(BitmapFrame.Create(image));
         using (var stream = File.Create(path)) encoder.Save(stream);
      }

      // launch editorExe on the file, or the Windows default when no editor is configured/installed
      private static void Launch(string editorExe, string file)
      {
         try
         {
            if (!string.IsNullOrEmpty(editorExe) && File.Exists(editorExe)) Process.Start(editorExe, "\"" + file + "\"");
            else Process.Start(new ProcessStartInfo(file) { UseShellExecute = true });
         }
         catch (Exception ex) { MessageBox.Show("Couldn't open the image:\n" + ex.Message, "Patch Studio"); }
      }

   }
}
