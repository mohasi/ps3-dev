using System.ComponentModel;

namespace RcoStudio
{
   public enum JobStatus { Pending, Dumping, Dumped, Compiling, Compiled, Failed }

   // one rco being worked on; rows of the main list bind to this
   public class RcoJob : INotifyPropertyChanged
   {
      public string Name { get; set; }              // "system_plugin"
      public string RcoPath { get; set; }           // source .rco (empty for dumps found on disk)
      public string DumpDir { get; set; }           // dumps\<name>
      public bool HeaderCompressed { get; set; }    // learned during dump, reused on compile
      public volatile bool Removed;                 // set when cleared; queued work checks and skips

      private bool isChecked;
      public bool IsChecked { get { return isChecked; } set { isChecked = value; Notify("IsChecked"); } }

      private JobStatus status;
      public JobStatus Status { get { return status; } set { status = value; Notify("Status"); } }

      private string detail = "";
      public string Detail { get { return detail; } set { detail = value; Notify("Detail"); } }

      // any file in this dump changed since it was dumped -- marks the row, so it is visible
      // without opening each rco to look
      private bool hasEdits;
      public bool HasEdits { get { return hasEdits; } set { hasEdits = value; Notify("HasEdits"); } }

      public event PropertyChangedEventHandler PropertyChanged;
      private void Notify(string property)
      {
         var handler = PropertyChanged;
         if (handler != null) handler(this, new PropertyChangedEventArgs(property));
      }
   }
}
