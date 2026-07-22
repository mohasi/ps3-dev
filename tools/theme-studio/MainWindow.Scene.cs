using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using ICSharpCode.AvalonEdit.Document;

namespace ThemeStudio
{
   // the 3D scene tab: a list of what exists on the left, the script that drives it on the right.
   // the split follows the format itself -- a script can change objects but can never create or
   // delete them, so the list has to come first.
   public partial class MainWindow
   {
      private SceneProject scene { get { return project.Scene; } }
      private bool settingScriptText;   // set while filling the box in, so it does not write back

      // the last text this window put in the script box. anything else means the user has typed,
      // and typed text is never thrown away.
      private string scriptTextShown = "";

      // what each row of the list stands for. the camera and the lights are in every scene and
      // hold no slot in Actors, so their rows point at nothing.
      private readonly List<SceneActor> actorByRow = new List<SceneActor>();

      private SceneActor selectedActor
      {
         get
         {
            int index = actorList.SelectedIndex;
            return index >= 0 && index < actorByRow.Count ? actorByRow[index] : null;
         }
      }

      private void showScene()
      {
         scene.EnsureLights();
         SceneDefaults.Fill(scene, project.ResolveAsset);   // so the list can say what each model asks for

         int keepIndex = actorList.SelectedIndex;
         actorList.Items.Clear();
         actorByRow.Clear();

         addRow(makeCameraRow(), null);
         foreach (SceneLight light in scene.Lights) addRow(makeLightRow(light), null);
         foreach (SceneActor actor in scene.Actors) addRow(makeActorRow(actor), actor);

         if (keepIndex >= 0 && keepIndex < actorList.Items.Count) actorList.SelectedIndex = keepIndex;
         else if (actorList.Items.Count > 0) actorList.SelectedIndex = 0;

         refreshStarterUnlessEdited();
      }

      private void addRow(StackPanel row, SceneActor actor)
      {
         actorList.Items.Add(row);
         actorByRow.Add(actor);
      }

      // the camera and the lights used to be a settings dialog, which made a scriptable thing look
      // like a fixed one. they are listed instead, named the way a script has to name them.
      private StackPanel makeCameraRow()
      {
         return makeRow(SceneProject.CameraId + " (fixed)", "the view itself   -   move it, turn it, widen it",
                        Colors.Transparent);
      }

      private StackPanel makeLightRow(SceneLight light)
      {
         string what = light.Type == "ambient" ? "an even glow from everywhere   -   colour"
                                               : "the main light   -   position, colour, falloff";
         return makeRow(light.Id + " (fixed)", what, ScenePreview.ToColor(light.Color));
      }

      private StackPanel makeActorRow(SceneActor actor)
      {
         SceneModel model = scene.FindModel(actor.ModelId);
         SceneMaterial material = scene.FindMaterial(actor.MaterialId);

         var parts = new List<string>();
         if (model != null) parts.Add(Path.GetFileName(model.DaePath));
         if (material != null) parts.Add(SceneEffects.ToPlainName(material.Effect));
         if (model != null) parts.Add(describePlacement(model));

         return makeRow(actor.Id, string.Join("   -   ", parts.ToArray()), Colors.Transparent);
      }

      // a model brings its own starting place and size, and the console uses them. saying so here
      // is what stops "why is my object off screen" being a mystery -- the numbers are the answer.
      private static string describePlacement(SceneModel model)
      {
         if (!model.AsksToBePlaced) return "sits where it is put";
         var said = new List<string>();
         if (model.DrawnSize > 0) said.Add("drawn " + round(model.DrawnSize) + " across");
         if (Math.Abs(model.DefaultPosition.X) > 0.001 || Math.Abs(model.DefaultPosition.Y) > 0.001 ||
             Math.Abs(model.DefaultPosition.Z) > 0.001)
            said.Add("starts at " + round(model.DefaultPosition.X) + ", " + round(model.DefaultPosition.Y) +
                     ", " + round(model.DefaultPosition.Z));
         return "asks to be " + string.Join(", ", said.ToArray());
      }

      private static string round(double value)
      {
         return value.ToString("0.#", System.Globalization.CultureInfo.InvariantCulture);
      }

      // an object is fixed once added, so a row reports what it is made of rather than offering
      // controls that could change it. the detail goes on a second line to keep this list the
      // same height as the pictures list beside it.
      private static StackPanel makeRow(string title, string detail, Color dot)
      {
         var heading = new DockPanel();
         if (dot.A > 0)
            heading.Children.Add(new System.Windows.Shapes.Ellipse {
               Width = 9, Height = 9, Fill = new SolidColorBrush(dot), VerticalAlignment = VerticalAlignment.Center,
               Margin = new Thickness(0, 0, 6, 0)
            });
         heading.Children.Add(new TextBlock { Text = title, TextWrapping = TextWrapping.Wrap });

         var row = new StackPanel();
         row.Children.Add(heading);
         row.Children.Add(new TextBlock {
            Text = detail, FontSize = 11, Opacity = 0.6,
            Margin = new Thickness(0, 2, 0, 0), TextWrapping = TextWrapping.Wrap
         });
         return row;
      }

      // adding and removing things

      private void onAddSceneModel(object sender, RoutedEventArgs e)
      {
         if (scene.Actors.Count >= SceneProject.MaxActors) {
            log("a scene can hold at most " + SceneProject.MaxActors + " things");
            return;
         }

         AddObjectWindow chosen = AddObjectWindow.Ask(this, new HashSet<string>(SceneBuild.UsedIds(scene)));
         if (chosen == null) return;

         DaeInfo info;
         if (!DaeFile.TryRead(chosen.ModelPath, out info)) {
            log("could not read " + Path.GetFileName(chosen.ModelPath));
            return;
         }

         string id = chosen.ObjectName;
         scene.Models.Add(new SceneModel { Id = id, DaePath = chosen.ModelPath, HasAnimation = info.HasAnimation });
         scene.Materials.Add(new SceneMaterial {
            Id = "mtrl_" + id, Effect = chosen.Lighting, TexturePath = chosen.TexturePath
         });

         // the object is left exactly as its model asks. the fit goes into the script instead,
         // where it can be read and changed -- setting it here would silently replace whatever
         // the model itself asks for, and nothing on screen would say why.

         scene.Actors.Add(new SceneActor { Id = id, ModelId = id, MaterialId = "mtrl_" + id });
         scene.EnsureLights();

         actorList.SelectedIndex = scene.Actors.Count - 1;
         showScene();
         appendPlacementToScript(id, info);
         refreshPreview();
         log("added " + Path.GetFileName(chosen.ModelPath) + " as \"" + id +
             "\" -- the lines that size and place it are at the end of the script");
      }

      // What the model asks for, written out as script rather than applied behind the scenes.
      // A model that already says where it belongs is put there and nothing is added on top; one
      // that says nothing gets a starting size and corner, written out in the same visible way.
      private void appendPlacementToScript(string id, DaeInfo model)
      {
         var text = new StringBuilder();
         text.Append("\r\n");
         text.Append(PsjsSnippets.MakePlacement(id, model, scene, scene.Actors.Count - 1));

         scriptBox.AppendText(text.ToString());
         scriptBox.ScrollToEnd();
      }

      private void onRemoveActor(object sender, RoutedEventArgs e)
      {
         SceneActor actor = selectedActor;
         if (actor == null) {
            log("every scene has a camera and two lights, so those cannot be removed -- " +
                "a script can move them, recolour them, or turn a light off");
            return;
         }
         scene.Actors.Remove(actor);
         showScene();
         refreshPreview();
         log("removed \"" + actor.Id + "\" -- any script line using it will now fail");
      }

      // Edit changes what a selected object is made of -- its model, texture or lighting -- keeping
      // the same name, so the script still refers to it. Its placement is not touched: that lives in
      // the script and is the user's, not something the makeup should move.
      private void onEditSelected(object sender, RoutedEventArgs e)
      {
         SceneActor actor = selectedActor;
         if (actor == null) { log("pick an object in the list to edit"); return; }

         SceneModel model = scene.Models.Find(candidate => candidate.Id == actor.ModelId);
         SceneMaterial material = scene.Materials.Find(candidate => candidate.Id == actor.MaterialId);
         if (model == null || material == null) { log("that object is missing its model or material"); return; }

         // every id in use except this object's own, so renaming it to the same name is not read
         // as a clash but renaming it onto another object's name still is
         var otherNames = new HashSet<string>(SceneBuild.UsedIds(scene));
         otherNames.Remove(actor.Id);

         // the dialog works in real file paths, so the stored (project-relative) ones are resolved
         // on the way in and compared against on the way out to tell a genuine change from none
         string currentModel = project.ResolveAsset(model.DaePath);
         string currentTexture = project.ResolveAsset(material.TexturePath);

         AddObjectWindow edited = AddObjectWindow.Edit(this, otherNames, actor.Id, currentModel,
                                                       currentTexture, material.Effect);
         if (edited == null) return;

         if (edited.ModelPath != currentModel) {
            model.DaePath = edited.ModelPath;
            DaeInfo info;
            model.HasAnimation = DaeFile.TryRead(edited.ModelPath, out info) && info.HasAnimation;
         }
         if (edited.TexturePath != currentTexture) material.TexturePath = edited.TexturePath;
         material.Effect = edited.Lighting;

         // the name a script uses is the actor's own id; the model and material ids are internal
         // plumbing the script never sees, so a rename only has to move the actor's id
         string oldId = actor.Id;
         actor.Id = edited.ObjectName;

         showScene();
         refreshPreview();
         log(actor.Id == oldId ? "updated \"" + actor.Id + "\""
                               : "renamed \"" + oldId + "\" to \"" + actor.Id + "\" -- update any script line using the old name");
      }

      // the script

      // called when a project is opened: whatever it holds replaces whatever was on screen
      private void showScript()
      {
         string path = project.ResolveAsset(scene.ScriptPath);
         setScriptText(path.Length > 0 && File.Exists(path) ? File.ReadAllText(path)
                                                            : PsjsSnippets.MakeStarter(scene.Actors));
      }

      // the starter text names the objects in the scene, so it is worth regenerating when they
      // change -- but only while the user has not written anything of their own
      private void refreshStarterUnlessEdited()
      {
         if (scene.ScriptPath.Length > 0) return;             // there is a real script to keep
         if (scriptBox.Text != scriptTextShown) return;       // the user has typed over it
         setScriptText(PsjsSnippets.MakeStarter(scene.Actors));
      }

      private void setScriptText(string text)
      {
         settingScriptText = true;
         scriptBox.Text = text;
         settingScriptText = false;
         scriptTextShown = text;
      }

      private void onScriptChanged(object sender, EventArgs e)
      {
         if (settingScriptText) return;
         scriptStatus.Text = "";
      }

      // lives beside the project as an ordinary file, and is written whenever the project is:
      // the editor's text being the only copy is how a script gets lost
      private string saveScript()
      {
         if (findProjectSceneBackground() == null) return "";   // no 3D scene, so no script to keep
         if (project.ContentDir.Length == 0) project.ContentDir = ProjectPackage.MakeWorkDir();

         // the script lives inside the project, like every other part of it. it is written on
         // every save because the editor's text box being the only copy is how a script gets lost.
         if (scene.ScriptPath.Length == 0) scene.ScriptPath = "scene.js";
         string path = project.ResolveAsset(scene.ScriptPath);
         Directory.CreateDirectory(Path.GetDirectoryName(path));
         File.WriteAllText(path, scriptBox.Text, Encoding.ASCII);
         scriptTextShown = scriptBox.Text;
         return path;
      }

      private void insertExample(PsjsExample example)
      {
         string text = example.Build(scene.Actors);
         int at = scriptBox.SelectionStart;
         scriptBox.Document.Replace(at, scriptBox.SelectionLength, text);
         scriptBox.CaretOffset = at + text.Length;
         scriptBox.Focus();
      }

      // runs Sony's own compiler, the only thing that can say whether the console will accept it.
      // the answer stays on this screen rather than going to the log.
      private void onCheckScript(object sender, RoutedEventArgs e)
      {
         if (scriptBox.Text.Trim().Length == 0) {
            showScriptStatus(true, "nothing written yet, which is fine -- the scene will simply sit still");
            return;
         }
         string path = saveScript();
         if (path.Length == 0) {
            showScriptStatus(false, "save the project first, so the script has somewhere to live");
            return;
         }
         string message;
         if (PsjsCheck.TryCompile(path, out message)) showScriptStatus(true, "the console will accept this");
         else showScriptProblem(message);
      }

      // the compiler says where it fell over ("scene.js:12: error: syntax error"). the line number
      // is the useful part of that, so the message says it in words and the box goes there.
      private void showScriptProblem(string complaint)
      {
         string text = lastLine(complaint);
         Match found = Regex.Match(text, @":(\d+):\s*(?:error:)?\s*(.*)$");
         int line;
         if (!found.Success || !int.TryParse(found.Groups[1].Value, out line)) {
            showScriptStatus(false, text);
            return;
         }

         showScriptStatus(false, "line " + line + ": " + found.Groups[2].Value);
         goToScriptLine(line);
      }

      private void goToScriptLine(int line)
      {
         if (line < 1 || line > scriptBox.Document.LineCount) return;   // a line the script does not have
         DocumentLine found = scriptBox.Document.GetLineByNumber(line);
         scriptBox.Focus();
         scriptBox.Select(found.Offset, found.Length);
         scriptBox.ScrollToLine(line);
      }

      private void showScriptStatus(bool good, string message)
      {
         scriptStatus.Text = (good ? "✓  " : "✗  ") + message;
         scriptStatus.Foreground = good ? GoodBrush : BadBrush;
      }

      // the compiler prints what it was doing first and what actually went wrong last
      private static string lastLine(string text)
      {
         string[] lines = text.Split(new[] { (char)13, (char)10 }, StringSplitOptions.RemoveEmptyEntries);
         return lines.Length > 0 ? lines[lines.Length - 1].Trim() : "there is a problem";
      }

      // the popup offers itself after a dot or an arrow; Ctrl+Space asks for it anywhere else
      private void onScriptKeyDown(object sender, KeyEventArgs e)
      {
         if (e.Key == Key.Space && (Keyboard.Modifiers & ModifierKeys.Control) == ModifierKeys.Control) {
            scriptEditor.ShowSuggestions();
            e.Handled = true;
         }
      }
   }
}
