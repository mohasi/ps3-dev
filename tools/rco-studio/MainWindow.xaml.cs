using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace RcoStudio
{
   public partial class MainWindow : System.Windows.Window
   {
      private readonly ObservableCollection<RcoJob> jobs = new ObservableCollection<RcoJob>();
      private readonly BlockingCollection<Action> workQueue = new BlockingCollection<Action>();
      private readonly ObservableCollection<PreviewItem> previewItems = new ObservableCollection<PreviewItem>();
      private readonly DispatcherTimer searchTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(350) };
      private int previewVersion;   // bumped whenever the preview should restart; stale loads check it

      public MainWindow()
      {
         InitializeComponent();
         AppSettings.Load();
         jobList.ItemsSource = jobs;
         jobs.CollectionChanged += OnJobsChanged;
         previewList.ItemsSource = previewItems;
         searchTimer.Tick += (sender, e) => { searchTimer.Stop(); RefreshPreview(); };

         var worker = new Thread(WorkLoop) { IsBackground = true };
         worker.Start();

         LoadExistingDumps();
         SetupInfoColumnStretch();
         Activated += (sender, e) => { RefreshPreview(); RefreshEditedFlags(); };   // catch edits saved in external editors

         // .rco paths on the command line are added straight away (also used for automation)
         var commandLine = Environment.GetCommandLineArgs();
         if (commandLine.Length > 1)
         {
            var startupPaths = new string[commandLine.Length - 1];
            Array.Copy(commandLine, 1, startupPaths, 0, startupPaths.Length);
            AddRcos(startupPaths);
         }
      }

      // section: the Info column absorbs whatever width the resizable columns leave over
      private void SetupInfoColumnStretch()
      {
         jobList.SizeChanged += (sender, e) => StretchInfoColumn();
         var widthProperty = DependencyPropertyDescriptor.FromProperty(GridViewColumn.WidthProperty, typeof(GridViewColumn));
         widthProperty.AddValueChanged(checkColumn, (sender, e) => StretchInfoColumn());
         widthProperty.AddValueChanged(nameColumn, (sender, e) => StretchInfoColumn());
         widthProperty.AddValueChanged(statusColumn, (sender, e) => StretchInfoColumn());
      }

      private void StretchInfoColumn()
      {
         double remaining = jobList.ActualWidth - checkColumn.ActualWidth - nameColumn.ActualWidth - statusColumn.ActualWidth - 30;
         if (remaining > 80) infoColumn.Width = remaining;
      }

      // section: checked rows select what Compile/Clear act on; none checked = all
      private void OnJobsChanged(object sender, NotifyCollectionChangedEventArgs e)
      {
         if (e.NewItems != null)
            foreach (RcoJob job in e.NewItems) job.PropertyChanged += OnJobPropertyChanged;
         UpdateActionLabels();
         emptyHint.Visibility = jobs.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
      }

      private void OnJobPropertyChanged(object sender, PropertyChangedEventArgs e)
      {
         if (e.PropertyName == "IsChecked") UpdateActionLabels();
      }

      // everything that reads or writes a dump is queued, so anything that would race it is
      // disabled until the queue drains: clearing folders mid-compile, patching a dump that is
      // still being written, exporting edits that are half applied.
      private void UpdateActionLabels()
      {
         bool busy = pendingWork > 0;
         bool anyChecked = CheckedJobs().Count > 0;
         bool anyJobs = jobs.Count > 0;

         compileButton.IsEnabled = anyChecked && !busy;
         clearButton.IsEnabled = anyChecked && !busy;

         // saving a set or exporting a patch both read the list, so they need one. applying a
         // patch does not: on an empty list it still names the rcos the patch wants.
         saveSetButton.IsEnabled = anyJobs && !busy;
         exportPatchButton.IsEnabled = anyJobs && !busy;
         applyPatchButton.IsEnabled = !busy;
         loadSetButton.IsEnabled = !busy;
         busyText.Text = "working… " + pendingWork + " job(s) left";
         busyText.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
      }

      // section: background work. one worker drains this queue in order, so queued jobs never
      // overlap; pendingWork is what the buttons above watch.
      private int pendingWork;

      private void QueueWork(Action work)
      {
         Interlocked.Increment(ref pendingWork);
         UpdateActionLabels();
         workQueue.Add(() =>
         {
            try { work(); }
            finally
            {
               Interlocked.Decrement(ref pendingWork);
               Dispatcher.BeginInvoke(new Action(UpdateActionLabels));
            }
         });
      }

      private List<RcoJob> CheckedJobs()
      {
         var checkedJobs = new List<RcoJob>();
         foreach (RcoJob job in jobs)
            if (job.IsChecked) checkedJobs.Add(job);
         return checkedJobs;
      }

      private void OnCheckAll(object sender, RoutedEventArgs e)
      {
         bool check = checkAllBox.IsChecked == true;
         foreach (RcoJob job in jobs) job.IsChecked = check;
      }

      // section: sorting by name on header click
      private ListSortDirection nameSortDirection = ListSortDirection.Descending;

      private void OnColumnHeaderClick(object sender, RoutedEventArgs e)
      {
         var header = e.OriginalSource as GridViewColumnHeader;
         if (header == null || header.Column != nameColumn) return;

         nameSortDirection = nameSortDirection == ListSortDirection.Ascending ? ListSortDirection.Descending : ListSortDirection.Ascending;
         var view = CollectionViewSource.GetDefaultView(jobs);
         view.SortDescriptions.Clear();
         view.SortDescriptions.Add(new SortDescription("Name", nameSortDirection));
         nameColumn.Header = MakeSortedHeader("Name", nameSortDirection);
      }

      // explorer-style sort chevron: wpf has no built-in glyph, this is the Segoe MDL2 one
      private static object MakeSortedHeader(string text, ListSortDirection direction)
      {
         var panel = new StackPanel { Orientation = Orientation.Horizontal };
         panel.Children.Add(new TextBlock { Text = text });
         panel.Children.Add(new TextBlock
         {
            Text = direction == ListSortDirection.Ascending ? "\uE70E" : "\uE70D",  // Segoe MDL2 ChevronUp / ChevronDown
            FontFamily = new System.Windows.Media.FontFamily("Segoe MDL2 Assets"),
            FontSize = 9,
            Margin = new Thickness(6, 3, 0, 0)
         });
         return panel;
      }

      // section: adding rcos (button + drag-drop), dumping starts immediately
      private void OnAddRcos(object sender, RoutedEventArgs e)
      {
         var dialog = new Microsoft.Win32.OpenFileDialog { Filter = "RCO files (*.rco)|*.rco", Multiselect = true };
         if (dialog.ShowDialog(this) == true) AddRcos(dialog.FileNames);
      }

      private void OnDrop(object sender, DragEventArgs e)
      {
         var paths = e.Data.GetData(DataFormats.FileDrop) as string[];
         if (paths != null) AddRcos(paths);
      }

      private void AddRcos(string[] paths)
      {
         int kept = 0;
         foreach (string path in ExpandFolders(paths))
         {
            if (!path.EndsWith(".rco", StringComparison.OrdinalIgnoreCase)) continue;

            string name = Path.GetFileNameWithoutExtension(path);
            RcoJob job = FindJob(name) ?? NewJob(name);

            // already dumped from this very file -- keep the dump, any edits in it, and whatever
            // was compiled from it, instead of redoing the work and stranding the compiled rco
            if (IsDumpedFrom(job, path)) { job.RcoPath = path; RestoreJobState(job); kept++; continue; }

            // a re-dump from a DIFFERENT source replaces the dump wholesale; if the old dump has
            // edits, they belong to the old source and cannot carry over, so let the user cancel
            if (Directory.Exists(job.DumpDir) && ToolRunner.FindEditedFiles(job.DumpDir).Count > 0)
            {
               var choice = MessageBox.Show(this,
                  job.Name + " already has a dump with edits, from a different source file.\n\n" +
                  "Re-dumping from this file replaces it and discards those edits (they belong to the old file). Continue?",
                  "Re-dump " + job.Name, MessageBoxButton.OKCancel, MessageBoxImage.Warning);
               if (choice != MessageBoxResult.OK) continue;
            }

            job.RcoPath = path;
            job.Removed = false;
            job.Status = JobStatus.Pending;
            job.Detail = "";
            RcoJob queuedJob = job;
            QueueWork(() => DumpJob(queuedJob));
         }
         if (kept > 0) Log(kept + " rco(s) already dumped from the same file — left as they are (Clear them first to dump again)");
         RefreshEditedFlags();
      }

      private static bool IsDumpedFrom(RcoJob job, string sourcePath)
      {
         if (!File.Exists(Path.Combine(job.DumpDir, job.Name + ".xml"))) return false;
         return string.Equals(ToolRunner.ReadSavedSourcePath(job.DumpDir), sourcePath, StringComparison.OrdinalIgnoreCase);
      }

      // what a dump on disk already tells us: how to compile it, and whether it has been
      private static void RestoreJobState(RcoJob job)
      {
         job.HeaderCompressed = ToolRunner.ReadSavedHeaderCompression(job.DumpDir);
         string compiledRco = Path.Combine(ToolRunner.CompiledDir, job.Name + ".rco");
         if (File.Exists(compiledRco)) { job.Status = JobStatus.Compiled; job.Detail = compiledRco; }
         else { job.Status = JobStatus.Dumped; job.Detail = ""; }
      }

      // a dropped folder means every .rco inside it; a dropped set means the rcos it lists
      private List<string> ExpandFolders(string[] paths)
      {
         var expanded = new List<string>();
         foreach (string path in paths)
         {
            if (Directory.Exists(path)) expanded.AddRange(Directory.GetFiles(path, "*.rco"));
            else if (path.EndsWith(".rcoset", StringComparison.OrdinalIgnoreCase)) expanded.AddRange(ReadSet(path));
            else expanded.Add(path);
         }
         return expanded;
      }

      // the rco paths a .rcoset lists, minus any that have since moved or been deleted
      private List<string> ReadSet(string setFile)
      {
         var found = new List<string>();
         foreach (string line in File.ReadAllLines(setFile))
         {
            string path = line.Trim();
            if (path == "") continue;
            if (File.Exists(path)) found.Add(path);
            else Log("[warn] set entry missing on disk, skipped: " + path);
         }
         return found;
      }

      // section: compiling. jobs still dumping are fine to queue -- the queue runs
      // one thing at a time, so their compile simply starts after the dump finishes.
      private void OnCompile(object sender, RoutedEventArgs e)
      {
         int queued = 0;
         foreach (RcoJob job in CheckedJobs())
         {
            if (job.Status == JobStatus.Failed) continue;
            RcoJob queuedJob = job;
            QueueWork(() => CompileJob(queuedJob));
            queued++;
         }
         Log(queued > 0 ? "compile queued for " + queued + " rco(s)" : "nothing to compile");
      }

      // section: clearing
      private void OnClear(object sender, RoutedEventArgs e)
      {
         var targets = new List<RcoJob>();
         foreach (RcoJob job in CheckedJobs())
            if (job.Status != JobStatus.Dumping && job.Status != JobStatus.Compiling) targets.Add(job);
         if (targets.Count == 0) { Log("nothing to clear"); return; }

         var choice = MessageBox.Show(this,
            "Also delete their dump folders and compiled .rco files from disk?\n\nNo = only clear the list, files stay.",
            "Clear " + targets.Count + " item(s)", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
         if (choice == MessageBoxResult.Cancel) return;

         foreach (RcoJob job in targets)
         {
            job.Removed = true;
            if (choice == MessageBoxResult.Yes) DeleteJobFiles(job);
            jobs.Remove(job);
         }
         checkAllBox.IsChecked = false;
         logBox.Clear();
         RefreshPreview();   // the cleared rco may still be on show; its files are gone
      }

      private void DeleteJobFiles(RcoJob job)
      {
         try
         {
            if (Directory.Exists(job.DumpDir)) Directory.Delete(job.DumpDir, true);
            string compiledRco = Path.Combine(ToolRunner.CompiledDir, job.Name + ".rco");
            if (File.Exists(compiledRco)) File.Delete(compiledRco);
         }
         catch (Exception exception)
         {
            Log("[warn] could not fully delete files for " + job.Name + ": " + exception.Message);
         }
      }

      // section: background work
      private void WorkLoop()
      {
         foreach (Action work in workQueue.GetConsumingEnumerable()) work();
      }

      private void DumpJob(RcoJob job)
      {
         if (job.Removed) return;
         try
         {
            job.Status = JobStatus.Dumping;
            Log("dumping " + job.Name + "...");
            ToolRunner.Dump(job, Log);
            job.Status = JobStatus.Dumped;
            job.HasEdits = false;   // a fresh dump is by definition unedited
            Log("dumped " + job.Name + " (" + job.Detail + ")");
            Dispatcher.BeginInvoke(new Action(() => { if (jobList.SelectedItem == job) RefreshPreview(); }));
         }
         catch (Exception exception)
         {
            job.Status = JobStatus.Failed;
            job.Detail = "dump failed";
            Log("[error] " + job.Name + ": " + exception.Message);
         }
      }

      private void CompileJob(RcoJob job)
      {
         if (job.Removed) return;
         try
         {
            job.Status = JobStatus.Compiling;
            Log("compiling " + job.Name + "...");
            ToolRunner.CompileResult result = ToolRunner.Compile(job, Log);

            job.Status = JobStatus.Compiled;
            if (result.VerifyProblem == null)
            {
               job.Detail = "verified ✓  " + result.OutputRco;
               Log("compiled + verified " + job.Name + " -> " + result.OutputRco);
            }
            else
            {
               job.Detail = "verify: " + result.VerifyProblem;
               Log("[warn] " + job.Name + " compiled but verify found differences: " + result.VerifyProblem);
            }
         }
         catch (Exception exception)
         {
            job.Status = JobStatus.Failed;
            job.Detail = "compile failed";
            Log("[error] " + job.Name + ": " + exception.Message);
         }
      }

      // section: dumps on disk reappear on launch, showing whether they were already compiled
      private void LoadExistingDumps()
      {
         if (!Directory.Exists(ToolRunner.DumpsDir)) return;
         foreach (string dumpDir in Directory.GetDirectories(ToolRunner.DumpsDir))
         {
            string name = Path.GetFileName(dumpDir);
            if (name.StartsWith(".") || !File.Exists(Path.Combine(dumpDir, name + ".xml"))) continue;

            RcoJob job = NewJob(name);
            job.RcoPath = ToolRunner.ReadSavedSourcePath(dumpDir);
            RestoreJobState(job);
         }
         RefreshEditedFlags();
      }

      // section: helpers
      private RcoJob FindJob(string name)
      {
         foreach (RcoJob job in jobs)
            if (job.Name == name) return job;
         return null;
      }

      private RcoJob NewJob(string name)
      {
         var job = new RcoJob { Name = name, RcoPath = "", DumpDir = Path.Combine(ToolRunner.DumpsDir, name) };
         jobs.Add(job);
         return job;
      }

      // section: preview pane. no search text = the selected rco's images and sounds;
      // with search text = matches across every dump (file names + text content)
      private void OnJobSelectionChanged(object sender, SelectionChangedEventArgs e)
      {
         if (GetSearchText() == "") RefreshPreview();
      }

      private void OnSearchChanged(object sender, TextChangedEventArgs e)
      {
         searchPlaceholder.Visibility = searchBox.Text.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
         searchTimer.Stop();
         searchTimer.Start();
      }

      private string GetSearchText() { return searchBox.Text.Trim(); }

      private const string PreviewHintDefault = "select a dumped rco to browse its files — double-click opens (sounds play), right-click to Edit or Revert";

      private void RefreshPreview()
      {
         int version = Interlocked.Increment(ref previewVersion);
         previewItems.Clear();

         string searchText = GetSearchText();
         if (searchText != "") { previewHint.Visibility = Visibility.Collapsed; SearchAllDumps(searchText, version); return; }

         previewHint.Text = PreviewHintDefault;
         var job = jobList.SelectedItem as RcoJob;
         if (job == null || !Directory.Exists(job.DumpDir)) { previewHint.Visibility = Visibility.Visible; return; }

         previewHint.Visibility = Visibility.Collapsed;
         string dumpDir = job.DumpDir;
         ThreadPool.QueueUserWorkItem(ignored => RunPreviewScan(dumpDir, version));
      }

      // the dump folder can be deleted or rewritten under us while this runs -- Clear removes it,
      // a re-dump replaces its files. an unhandled throw here would kill the app from a background
      // thread, and there is nothing to report: the scan is stale, so let the next one draw.
      private void RunPreviewScan(string dumpDir, int version)
      {
         try
         {
            // documents first (the structure xml and any embedded txt/xml), then images, then sounds
            var editedFiles = new HashSet<string>(ToolRunner.FindEditedFiles(dumpDir), StringComparer.OrdinalIgnoreCase);
            var documents = new List<string>(); var images = new List<string>(); var sounds = new List<string>();
            foreach (string file in Directory.GetFiles(dumpDir))
            {
               if (!IsPreviewable(file)) continue;
               string extension = Path.GetExtension(file).ToLowerInvariant();
               if (IsImageExtension(extension)) images.Add(file);
               else if (extension == ".wav") sounds.Add(file);
               else documents.Add(file);
            }
            foreach (string file in documents) AddPreviewItem(version, MarkEdited(MakeDocumentItem(file, Path.GetFileName(file)), editedFiles));
            foreach (string file in images) AddPreviewItem(version, MarkEdited(MakeImageItem(file, Path.GetFileNameWithoutExtension(file)), editedFiles));
            foreach (string file in sounds) AddPreviewItem(version, MarkEdited(MakeSoundItem(file, Path.GetFileNameWithoutExtension(file)), editedFiles));

            // gims with no png sibling (e.g. palette-less osk images GimConv can't decode) still get a tile
            foreach (string gimFile in Directory.GetFiles(dumpDir, "*.gim"))
               if (!File.Exists(Path.ChangeExtension(gimFile, ".png")))
                  AddPreviewItem(version, MakeRawImageItem(gimFile));
         }
         catch (IOException) { }
         catch (UnauthorizedAccessException) { }
      }

      private static PreviewItem MarkEdited(PreviewItem item, HashSet<string> editedFiles)
      {
         item.IsEdited = editedFiles.Contains(item.FilePath);
         item.IsLossy = ToolRunner.IsLossyFormat(item.FilePath);
         return item;
      }

      private void SearchAllDumps(string searchText, int version)
      {
         ThreadPool.QueueUserWorkItem(ignored =>
         {
            if (!Directory.Exists(ToolRunner.DumpsDir)) return;
            int matches = 0;
            try
            {
               foreach (string dumpDir in Directory.GetDirectories(ToolRunner.DumpsDir))
               {
                  if (version != previewVersion || matches >= 400) break;
                  string rcoName = Path.GetFileName(dumpDir);
                  if (rcoName.StartsWith(".")) continue;

                  // match resource file names
                  var editedFiles = new HashSet<string>(ToolRunner.FindEditedFiles(dumpDir), StringComparer.OrdinalIgnoreCase);
                  foreach (string file in Directory.GetFiles(dumpDir))
                  {
                     if (!IsPreviewable(file)) continue;
                     string baseName = Path.GetFileNameWithoutExtension(file);
                     if (baseName.IndexOf(searchText, StringComparison.OrdinalIgnoreCase) < 0) continue;

                     string caption = rcoName + " · " + baseName;
                     string extension = Path.GetExtension(file).ToLowerInvariant();
                     PreviewItem item;
                     if (IsImageExtension(extension)) item = MakeImageItem(file, caption);
                     else if (extension == ".wav") item = MakeSoundItem(file, caption);
                     else item = MakeDocumentItem(file, caption);
                     AddPreviewItem(version, MarkEdited(item, editedFiles));
                     matches++;
                  }

                  // match text content inside the structure xml (labels, menu strings, object names)
                  string xmlFile = Path.Combine(dumpDir, rcoName + ".xml");
                  if (File.Exists(xmlFile))
                  {
                     int hits = CountOccurrences(File.ReadAllText(xmlFile), searchText);
                     if (hits > 0)
                     {
                        AddPreviewItem(version, MakeDocumentItem(xmlFile, rcoName + ".xml — " + hits + " hit(s)"));
                        matches++;
                     }
                  }
               }
            }
            catch (IOException) { }                    // a dump cleared or re-dumped mid-search
            catch (UnauthorizedAccessException) { }
            Dispatcher.BeginInvoke(new Action(() =>
            {
               if (version == previewVersion && previewItems.Count == 0)
               {
                  previewHint.Text = "no matches for \"" + searchText + "\" across the dumped rcos";
                  previewHint.Visibility = Visibility.Visible;
               }
            }));
         });
      }

      private void AddPreviewItem(int version, PreviewItem item)
      {
         Dispatcher.BeginInvoke(new Action(() => { if (version == previewVersion) previewItems.Add(item); }));
      }

      private static PreviewItem MakeImageItem(string file, string caption)
      {
         return new PreviewItem { FilePath = file, Caption = caption, ToolTipText = file, Thumbnail = LoadThumbnail(file) };
      }

      // previewable = everything except the console-format raws (shown via their siblings) and dump-info.txt
      private static bool IsPreviewable(string file)
      {
         return !ToolRunner.IsRawResource(file) && !Path.GetFileName(file).Equals("dump-info.txt", StringComparison.OrdinalIgnoreCase);
      }

      private static bool IsImageExtension(string extension)
      {
         return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".gif" || extension == ".tif";
      }

      private static PreviewItem MakeRawImageItem(string file)
      {
         return new PreviewItem { FilePath = file, Caption = Path.GetFileNameWithoutExtension(file), ToolTipText = file + " (console-format image with no png preview)", Glyph = "\uE91B" };  // MDL2 Photo
      }

      private static PreviewItem MakeSoundItem(string file, string caption)
      {
         return new PreviewItem { FilePath = file, Caption = caption, ToolTipText = file + " (double-click to play)", Glyph = "\uE767", IsSound = true };  // MDL2 Volume
      }

      private static PreviewItem MakeDocumentItem(string file, string caption)
      {
         return new PreviewItem { FilePath = file, Caption = caption, ToolTipText = file, Glyph = "\uE8A5" };  // MDL2 Document
      }

      // loads a small frozen thumbnail without keeping the file locked (edits/compiles rewrite these files)
      private static BitmapImage LoadThumbnail(string imageFile)
      {
         try
         {
            using (var stream = new FileStream(imageFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
               var bitmap = new BitmapImage();
               bitmap.BeginInit();
               bitmap.CacheOption = BitmapCacheOption.OnLoad;
               bitmap.StreamSource = stream;
               bitmap.DecodePixelWidth = 64;
               bitmap.EndInit();
               bitmap.Freeze();
               return bitmap;
            }
         }
         catch { return null; }
      }

      private static int CountOccurrences(string text, string term)
      {
         int count = 0, position = 0;
         while ((position = text.IndexOf(term, position, StringComparison.OrdinalIgnoreCase)) >= 0) { count++; position += term.Length; }
         return count;
      }

      // double-click: sounds play; everything else opens in the Windows default app.
      // (right-click -> Edit opens the editor configured in settings.txt)
      private void OnPreviewDoubleClick(object sender, MouseButtonEventArgs e)
      {
         var item = previewList.SelectedItem as PreviewItem;
         if (item == null) return;
         if (item.IsSound) { PlaySound(item); return; }
         OpenDefault(item);
      }

      private void PlaySound(PreviewItem item)
      {
         if (!File.Exists(item.FilePath)) return;
         try { new System.Media.SoundPlayer(item.FilePath).Play(); }
         catch (Exception exception) { Log("[warn] could not play " + item.Caption + ": " + exception.Message); }
      }

      private void OpenDefault(PreviewItem item)
      {
         if (!File.Exists(item.FilePath)) return;
         try { Process.Start(item.FilePath); }
         catch { Process.Start("explorer.exe", "/select,\"" + item.FilePath + "\""); }
      }

      // right-click selects the tile under the cursor so the context menu acts on it
      private void OnPreviewRightButtonDown(object sender, MouseButtonEventArgs e)
      {
         var element = e.OriginalSource as DependencyObject;
         while (element != null && !(element is ListBoxItem)) element = System.Windows.Media.VisualTreeHelper.GetParent(element);
         if (element is ListBoxItem) ((ListBoxItem)element).IsSelected = true;
      }

      private string EditorFor(PreviewItem item)
      {
         string extension = Path.GetExtension(item.FilePath).ToLowerInvariant();
         if (extension == ".png") return AppSettings.ImageEditor;
         if (extension == ".xml" || extension == ".txt" || extension == ".ini") return AppSettings.TextEditor;
         return "";   // sounds and other types have no configured editor
      }

      private void OnPreviewMenuOpening(object sender, RoutedEventArgs e)
      {
         var item = previewList.SelectedItem as PreviewItem;
         string editor = item == null ? "" : EditorFor(item);
         bool editorConfigured = editor != "" && File.Exists(editor);
         editMenuItem.IsEnabled = item != null && !item.IsSound && editorConfigured;
         editMenuItem.Header = item != null && !item.IsSound && !editorConfigured
            ? "Edit (set editor in settings.txt)" : "Edit";

         // revert undoes any change: restoring a dumped file, or removing one you added.
         // diff needs something to compare against, so it needs the dumped copy to exist.
         bool hasPristineCopy = item != null && ToolRunner.GetPristineCopy(item.FilePath) != "";
         bool diffToolConfigured = DiffToolInstalled();
         revertMenuItem.IsEnabled = item != null && item.IsEdited;
         compareMenuItem.IsEnabled = item != null && item.IsEdited && hasPristineCopy && diffToolConfigured;
         compareMenuItem.Header = diffToolConfigured ? "Diff" : "Diff (set diffTool in settings.txt)";
      }

      private void OnPreviewEdit(object sender, RoutedEventArgs e)
      {
         var item = previewList.SelectedItem as PreviewItem;
         if (item == null) return;
         string editor = EditorFor(item);
         if (editor == "" || !File.Exists(editor)) return;
         if (item.IsLossy && !WarnLossyOnce(item)) return;
         Process.Start(editor, "\"" + item.FilePath + "\"");
      }

      // section: adding resources an rco never had -- drop pngs/wavs onto the preview of a selected rco
      private void OnPreviewDrop(object sender, DragEventArgs e)
      {
         e.Handled = true;   // otherwise the window's rco drop handler sees this too
         var paths = e.Data.GetData(DataFormats.FileDrop) as string[];
         if (paths == null) return;

         var pngFiles = new List<string>();
         var wavFiles = new List<string>();
         foreach (string path in paths)
         {
            if (path.EndsWith(".png", StringComparison.OrdinalIgnoreCase)) pngFiles.Add(path);
            else if (path.EndsWith(".wav", StringComparison.OrdinalIgnoreCase)) wavFiles.Add(path);
         }

         // dropping anything else here still means what it means anywhere else in the window
         if (pngFiles.Count == 0 && wavFiles.Count == 0) { AddRcos(paths); return; }

         var job = jobList.SelectedItem as RcoJob;
         if (job == null || !Directory.Exists(job.DumpDir))
         {
            MessageBox.Show(this, "Select an RCO on the left first — a new resource has to be added to one.",
               "Add resource", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
         }

         // an rco can only hold what its plugin was built to use, so say so before asking
         // anything else -- there is no point picking a format for a drop that cannot land
         if (!WarnIfCannotHold(job, wavFiles, "SoundTree", "sounds")) return;
         if (!WarnIfCannotHold(job, pngFiles, "ImageTree", "images")) return;

         // only images need a format picked; a wav's channel count comes from the file itself
         var format = ToolRunner.GimFormat.Rgba8888;
         if (pngFiles.Count > 0)
         {
            var dialog = new AddImageWindow(this, job.Name, pngFiles, ToolRunner.SummariseImageFormats(job.DumpDir));
            if (dialog.ShowDialog() != true) return;
            format = dialog.SelectedFormat;
         }

         RcoJob targetJob = job;
         QueueWork(() => AddResources(targetJob, pngFiles, wavFiles, format));
      }

      // true to carry on. false when the rco has no tree for these files, which is not an error
      // the user can fix by trying again -- the rco simply has no way to reach them.
      private bool WarnIfCannotHold(RcoJob job, List<string> files, string treeName, string what)
      {
         if (files.Count == 0 || ToolRunner.HasTree(job, treeName)) return true;
         MessageBox.Show(this,
            job.Name + " has no " + what + " at all, so it has no way to reach one you add.\n\n" +
            "Only an RCO that already has " + what + " can take more — the XMB's " + what + " live in system_plugin.",
            "Cannot add " + what + " here", MessageBoxButton.OK, MessageBoxImage.Information);
         return false;
      }

      private void AddResources(RcoJob job, List<string> pngFiles, List<string> wavFiles, ToolRunner.GimFormat format)
      {
         int added = 0;
         foreach (string pngFile in pngFiles)
         {
            try { ToolRunner.AddImage(job, pngFile, format, Log); added++; }
            catch (Exception exception) { Log("[warn] could not add " + Path.GetFileName(pngFile) + ": " + exception.Message); }
         }
         foreach (string wavFile in wavFiles)
         {
            try { ToolRunner.AddSound(job, wavFile, Log); added++; }
            catch (Exception exception) { Log("[warn] could not add " + Path.GetFileName(wavFile) + ": " + exception.Message); }
         }
         Log("added " + added + " resource(s) to " + job.Name + " — compile to build them into the rco");
         Dispatcher.BeginInvoke(new Action(() => { RefreshPreview(); RefreshEditedFlags(); }));
      }

      private static bool DiffToolInstalled()
      {
         return AppSettings.DiffTool != "" && File.Exists(AppSettings.DiffTool);
      }

      // opens the configured compare tool with the dumped copy on the left and the edit on the right
      private void OnPreviewCompare(object sender, RoutedEventArgs e)
      {
         var item = previewList.SelectedItem as PreviewItem;
         if (item == null || !DiffToolInstalled()) return;
         string pristineFile = ToolRunner.GetPristineCopy(item.FilePath);
         if (pristineFile == "") return;
         try { Process.Start(AppSettings.DiffTool, "\"" + pristineFile + "\" \"" + item.FilePath + "\""); }
         catch (Exception exception) { Log("[warn] could not start the compare tool: " + exception.Message); }
      }

      private void OnPreviewRevert(object sender, RoutedEventArgs e)
      {
         var item = previewList.SelectedItem as PreviewItem;
         var job = jobList.SelectedItem as RcoJob;
         if (item == null || job == null) return;

         // reverting the xml also drops resources added since the dump, because the restored xml
         // no longer lists them -- deleting someone's new artwork deserves a warning first
         if (item.FilePath.Equals(Path.Combine(job.DumpDir, job.Name + ".xml"), StringComparison.OrdinalIgnoreCase))
         {
            List<string> added = ToolRunner.FindAddedResources(job);
            if (added.Count > 0)
            {
               var choice = MessageBox.Show(this,
                  "Reverting the structure XML also removes the " + added.Count + " image(s)/sound(s) you added, " +
                  "since the dumped XML has no entries for them.\n\nCarry on?",
                  "Revert " + job.Name + ".xml", MessageBoxButton.OKCancel, MessageBoxImage.Warning);
               if (choice != MessageBoxResult.OK) return;
            }
         }

         try
         {
            ToolRunner.RevertResource(item.FilePath, job, Log);
            RefreshPreview();
            RefreshEditedFlags();
         }
         catch (Exception exception) { Log("[warn] revert failed: " + exception.Message); }
      }

      // section: the amber marker on a row. checked off the dump on a background thread, since
      // it compares every file against its dumped copy.
      private void RefreshEditedFlags()
      {
         var targets = new List<RcoJob>(jobs);
         ThreadPool.QueueUserWorkItem(ignored =>
         {
            foreach (RcoJob job in targets)
            {
               if (job.Removed || !Directory.Exists(job.DumpDir)) continue;
               bool edited;
               try { edited = ToolRunner.FindEditedFiles(job.DumpDir).Count > 0; }
               catch (IOException) { continue; }              // being dumped or cleared right now
               catch (UnauthorizedAccessException) { continue; }
               RcoJob target = job;
               Dispatcher.BeginInvoke(new Action(() => target.HasEdits = edited));
            }
         });
      }

      // one warning per session before editing a lossy-format resource. returns false if the user cancels.
      private bool lossyWarningShown;
      private bool WarnLossyOnce(PreviewItem item)
      {
         if (lossyWarningShown) return true;
         lossyWarningShown = true;
         string kind = item.IsSound ? "a VAG sound" : "a DXT-compressed image";
         var choice = MessageBox.Show(this,
            "This is " + kind + " — a lossy console format. Each time an edit is compiled it re-encodes with a little quality loss.\n\n" +
            "Best practice: keep your change and share it with Export Patch. Applying a patch onto freshly-dumped originals re-encodes only once, so quality never stacks up.\n\nOpen the editor anyway?",
            "Lossy format", MessageBoxButton.OKCancel, MessageBoxImage.Information);
         return choice == MessageBoxResult.OK;
      }

      // section: sets (a saved list of source rco paths, for whole-theme workflows)
      private void OnSaveSet(object sender, RoutedEventArgs e)
      {
         var paths = new List<string>();
         foreach (RcoJob job in jobs)
            if (job.RcoPath != "") paths.Add(job.RcoPath);
         if (paths.Count == 0)
         {
            MessageBox.Show(this, "Nothing to save — add some RCOs first.", "Save Set", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
         }

         var dialog = new Microsoft.Win32.SaveFileDialog { Filter = "RCO set (*.rcoset)|*.rcoset", FileName = "my-theme.rcoset" };
         if (dialog.ShowDialog(this) != true) return;
         File.WriteAllLines(dialog.FileName, paths.ToArray());
         Log("saved set of " + paths.Count + " rco(s) to " + dialog.FileName);
      }

      private void OnLoadSet(object sender, RoutedEventArgs e)
      {
         var dialog = new Microsoft.Win32.OpenFileDialog { Filter = "RCO set (*.rcoset)|*.rcoset" };
         if (dialog.ShowDialog(this) != true) return;
         AddRcos(ReadSet(dialog.FileName).ToArray());
      }

      // section: patches (share only the changed files, apply someone else's onto your dumps)
      private void OnExportPatch(object sender, RoutedEventArgs e)
      {
         // checked rows, or every dumped rco when none are checked
         List<RcoJob> targets = CheckedJobs();
         bool usingChecked = targets.Count > 0;
         if (!usingChecked) targets = new List<RcoJob>(jobs);

         int editedCount = 0;
         foreach (RcoJob job in targets) editedCount += ToolRunner.FindEditedFiles(job.DumpDir).Count;
         if (editedCount == 0)
         {
            MessageBox.Show(this, "No edited files found in the " + (usingChecked ? "checked" : "dumped") + " RCOs.\n\nA patch contains only files you changed: edited pngs/wavs and hand-edited xml.", "Export Patch", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
         }

         var dialog = new Microsoft.Win32.SaveFileDialog { Filter = "RCO patch (*.rcopatch)|*.rcopatch", FileName = "my-mod.rcopatch" };
         if (dialog.ShowDialog(this) != true) return;

         int packed = PatchFile.Export(targets, dialog.FileName, Log);
         Log("exported patch with " + packed + " file(s) to " + dialog.FileName);
      }

      private void OnApplyPatch(object sender, RoutedEventArgs e)
      {
         var dialog = new Microsoft.Win32.OpenFileDialog { Filter = "RCO patch (*.rcopatch)|*.rcopatch" };
         if (dialog.ShowDialog(this) != true) return;

         // applying overwrites files in place with no undo, so warn if it would bury your own edits
         List<string> clobbered = PatchFile.FindEditsThatWouldBeOverwritten(dialog.FileName);
         if (clobbered.Count > 0)
         {
            var choice = MessageBox.Show(this,
               "This patch overwrites " + clobbered.Count + " file(s) you have edited yourself:\n\n  " +
               string.Join("\n  ", clobbered.ToArray()) +
               "\n\nYour edits will be lost — there is no undo. Continue?",
               "Apply Patch", MessageBoxButton.OKCancel, MessageBoxImage.Warning);
            if (choice != MessageBoxResult.OK) return;
         }

         List<string> missingRcos;
         int applied = PatchFile.Apply(dialog.FileName, Log, out missingRcos);
         Log("applied " + applied + " file(s) from " + dialog.FileName + (applied > 0 ? " — compile to build the modded rco(s)" : ""));
         RefreshPreview();
         RefreshEditedFlags();

         if (missingRcos.Count == 0 && applied > 0)
            MessageBox.Show(this, "Applied " + applied + " file(s).\n\nPress Compile to build the modded rco(s).", "Apply Patch", MessageBoxButton.OK, MessageBoxImage.Information);
         if (missingRcos.Count > 0)
            MessageBox.Show(this,
               "This patch modifies RCOs you haven't dumped yet:\n\n  " + string.Join("\n  ", missingRcos.ToArray()) +
               "\n\nAdd those .rco files from your firmware so they dump, then apply the patch again." +
               (applied > 0 ? "\n\n(" + applied + " file(s) for already-dumped RCOs were applied.)" : ""),
               "Apply Patch", MessageBoxButton.OK, MessageBoxImage.Warning);
      }

      private void OnRowDoubleClick(object sender, MouseButtonEventArgs e)
      {
         var job = jobList.SelectedItem as RcoJob;
         if (job != null && Directory.Exists(job.DumpDir)) Process.Start("explorer.exe", job.DumpDir);
      }

      private void OnOpenDumps(object sender, RoutedEventArgs e) { OpenFolder(ToolRunner.DumpsDir); }
      private void OnOpenCompiled(object sender, RoutedEventArgs e) { OpenFolder(ToolRunner.CompiledDir); }

      private static void OpenFolder(string path)
      {
         Directory.CreateDirectory(path);
         Process.Start("explorer.exe", path);
      }

      private void Log(string line)
      {
         Dispatcher.BeginInvoke(new Action(() =>
         {
            logBox.AppendText(line + Environment.NewLine);
            logBox.ScrollToEnd();
         }));
      }
   }
}
