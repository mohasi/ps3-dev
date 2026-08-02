using System.Windows;

namespace PatchStudio
{
   // collects the two things a project needs before its dump is downloaded: a name and the game ID
   // it patches. both must be non-empty to create.
   public partial class NewProjectDialog : Window
   {
      public string ProjectName { get { return nameBox.Text.Trim(); } }
      public string GameId { get { return gameIdBox.Text.Trim(); } }

      public NewProjectDialog(string suggestedName, string suggestedGameId = "")
      {
         InitializeComponent();
         nameBox.Text = suggestedName;
         gameIdBox.Text = suggestedGameId;
         nameBox.Focus();
         nameBox.SelectAll();
      }

      private void Create_Click(object sender, RoutedEventArgs e)
      {
         if (ProjectName == "" || GameId == "")
         {
            MessageBox.Show(this, "Enter a project name and a title ID.", "New Project");
            return;
         }
         DialogResult = true;
      }

      private void Cancel_Click(object sender, RoutedEventArgs e) { DialogResult = false; }
   }
}
