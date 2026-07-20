using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;

namespace ThemeStudio
{
   // everything about a new object in one place. the model, its picture and its lighting can never
   // be changed afterwards, so asking for them one dialog at a time hid the fact that they are one
   // decision -- and left no say over the name the script has to use.
   public partial class AddObjectWindow : Window
   {
      public string ModelPath { get; private set; }
      public string TexturePath { get; private set; }
      public string Lighting { get; private set; }
      public string ObjectName { get; private set; }

      private readonly HashSet<string> usedNames;

      private AddObjectWindow(HashSet<string> usedNames)
      {
         InitializeComponent();
         this.usedNames = usedNames;
         ModelPath = "";
         TexturePath = "";
         foreach (string plainName in SceneEffects.PlainNames) effectBox.Items.Add(plainName);
         effectBox.SelectedIndex = 0;
      }

      // adding a new object. returns null if the user backed out
      public static AddObjectWindow Ask(Window owner, HashSet<string> usedNames)
      {
         var dialog = new AddObjectWindow(usedNames) { Owner = owner };
         return dialog.ShowDialog() == true ? dialog : null;
      }

      // changing an object already in the scene. everything can change -- model, texture, lighting,
      // and the name -- so usedNames must NOT contain this object's own name, or renaming it to
      // itself would be treated as a clash. returns null if the user backed out.
      public static AddObjectWindow Edit(Window owner, HashSet<string> usedNames, string name,
                                         string modelPath, string texturePath, string effect)
      {
         var dialog = new AddObjectWindow(usedNames) { Owner = owner, Title = "Edit object" };
         dialog.headingText.Text = "Edit an object in the scene";
         dialog.hintText.Text = "Change the model, its texture, its lighting or the name. If you rename it, " +
                                "update any script line that refers to it by the old name.";
         dialog.nameBox.Text = name;
         dialog.ModelPath = modelPath;
         dialog.modelBox.Text = Path.GetFileName(modelPath);
         dialog.TexturePath = texturePath ?? "";
         dialog.textureBox.Text = string.IsNullOrEmpty(texturePath) ? "" : Path.GetFileName(texturePath);
         dialog.effectBox.SelectedItem = SceneEffects.ToPlainName(effect);
         dialog.addButton.Content = "Save";
         DaeInfo info;
         if (DaeFile.TryRead(modelPath, out info)) dialog.describeModel(info);
         dialog.showWhetherReady();   // the model and name are already filled, so Save starts enabled
         return dialog.ShowDialog() == true ? dialog : null;
      }

      private void onPickModel(object sender, RoutedEventArgs e)
      {
         string path = pickFile("3D model", "COLLADA models (*.dae)|*.dae");
         if (path == null) return;

         DaeInfo info;
         if (!DaeFile.TryRead(path, out info)) {
            modelInfo.Text = Path.GetFileName(path) + " could not be read -- is it a COLLADA model?";
            return;
         }

         ModelPath = path;
         modelBox.Text = Path.GetFileName(path);
         // a suggested name, so the common case needs no typing but can still be overridden
         if (nameBox.Text.Trim().Length == 0)
            nameBox.Text = SceneProject.MakeId(Path.GetFileNameWithoutExtension(path), usedNames);
         describeModel(info);
         showWhetherReady();
      }

      private void describeModel(DaeInfo info)
      {
         string text = info.VertexCount + " points";
         if (info.HasAnimation) text += ", and it has its own built-in animation, which will be included";
         text += ". It will be sized to sit in front of the camera.";
         modelInfo.Text = text;
      }

      private void onPickTexture(object sender, RoutedEventArgs e)
      {
         string path = pickFile("Texture", "dds textures (*.dds)|*.dds");
         if (path == null) return;
         TexturePath = path;
         textureBox.Text = Path.GetFileName(path);
      }

      private void onClearTexture(object sender, RoutedEventArgs e)
      {
         TexturePath = "";
         textureBox.Text = "";
      }

      private void onNameChanged(object sender, TextChangedEventArgs e) { showWhetherReady(); }

      private void showWhetherReady()
      {
         if (addButton == null) return;
         addButton.IsEnabled = ModelPath.Length > 0 && nameBox.Text.Trim().Length > 0;
      }

      private void onAccept(object sender, RoutedEventArgs e)
      {
         // the name becomes a name in a script, so it is cleaned the same way an automatic one is
         ObjectName = SceneProject.MakeId(nameBox.Text.Trim(), usedNames);
         Lighting = SceneEffects.FromPlainName((string)effectBox.SelectedItem);
         DialogResult = true;
      }

      private void onCancel(object sender, RoutedEventArgs e) { DialogResult = false; }

      private static string pickFile(string title, string filter)
      {
         var dialog = new OpenFileDialog { Title = title, Filter = filter };
         return dialog.ShowDialog() == true ? dialog.FileName : null;
      }
   }
}
