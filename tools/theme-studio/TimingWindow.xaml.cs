using System;
using System.Windows;
using System.Windows.Controls;

namespace ThemeStudio
{
   // when one background picture is on show. both ends are always set together: a picture with a
   // start and no end never appears, and nothing in the sdk tools catches that.
   //
   // the values are picked from lists rather than typed, so the stored text is always in the shape
   // the console expects (MMDD by date, HH by hour) and there is nothing to validate afterwards.
   public partial class TimingWindow : Window
   {
      public string From { get; private set; }
      public string Until { get; private set; }

      private readonly bool byDate;

      private static readonly string[] MonthNames = BackgroundTiming.MonthNames;
      // the longest each month can be, so 29 February can be chosen in a theme with no year
      private static readonly int[] MonthLengths = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

      private TimingWindow(bool byDate, string from, string until)
      {
         InitializeComponent();
         this.byDate = byDate;

         if (byDate) {
            headingLabel.Text = "Show this picture between two dates";
            hintLabel.Text = "The year is not part of it, so the same picture comes back every year. " +
                             "A range may run past the end of the year, for example 1 December to 5 January.";
            firstColumnLabel.Text = "Day";
            secondColumnLabel.Text = "Month";
            fillDayAndMonth(fromFirstBox, fromSecondBox);
            fillDayAndMonth(untilFirstBox, untilSecondBox);
         } else {
            headingLabel.Text = "Show this picture between two times of day";
            hintLabel.Text = "Whole hours only, on a 24-hour clock. A range may run past midnight, " +
                             "for example 22 to 06.";
            firstColumnLabel.Text = "Hour";
            fillHours(fromFirstBox);
            fillHours(untilFirstBox);
            fromSecondBox.Visibility = Visibility.Collapsed;
            untilSecondBox.Visibility = Visibility.Collapsed;
         }

         show(from, fromFirstBox, fromSecondBox);
         show(until, untilFirstBox, untilSecondBox);
         describe();
      }

      // returns null if the user backed out
      public static TimingWindow Ask(Window owner, bool byDate, string from, string until)
      {
         var dialog = new TimingWindow(byDate, from, until) { Owner = owner };
         return dialog.ShowDialog() == true ? dialog : null;
      }

      // filling the lists

      private static void fillDayAndMonth(ComboBox dayBox, ComboBox monthBox)
      {
         for (int day = 1; day <= 31; day++) dayBox.Items.Add(day.ToString());
         foreach (string month in MonthNames) monthBox.Items.Add(month);
      }

      private static void fillHours(ComboBox hourBox)
      {
         for (int hour = 0; hour < 24; hour++) hourBox.Items.Add(hour.ToString("00") + ":00");
      }

      // stored text <-> the lists

      private void show(string stored, ComboBox firstBox, ComboBox secondBox)
      {
         if (byDate) {
            int month, day;
            if (!tryReadDate(stored, out month, out day)) return;
            firstBox.SelectedIndex = day - 1;
            secondBox.SelectedIndex = month - 1;
            return;
         }
         int hour;
         if (int.TryParse(stored, out hour) && hour >= 0 && hour < 24) firstBox.SelectedIndex = hour;
      }

      private string read(ComboBox firstBox, ComboBox secondBox)
      {
         if (!byDate) return firstBox.SelectedIndex.ToString("00");
         int month = secondBox.SelectedIndex + 1;
         int day = firstBox.SelectedIndex + 1;
         return month.ToString("00") + day.ToString("00");
      }

      // a stored value may be MMDD or YYYYMMDD; both name the same day of the year
      private static bool tryReadDate(string stored, out int month, out int day)
      {
         month = day = 0;
         if (stored == null) return false;
         if (stored.Length == 8) stored = stored.Substring(4);
         if (stored.Length != 4) return false;
         return int.TryParse(stored.Substring(0, 2), out month) &&
                int.TryParse(stored.Substring(2, 2), out day) &&
                month >= 1 && month <= 12 && day >= 1 && day <= 31;
      }

      // what the choice means, and whether it can be accepted at all

      private void onAnyChanged(object sender, SelectionChangedEventArgs e) { describe(); }

      private void describe()
      {
         if (acceptButton == null) return;
         bool complete = isSet(fromFirstBox, fromSecondBox) && isSet(untilFirstBox, untilSecondBox);
         acceptButton.IsEnabled = complete && !isImpossibleDay();

         if (!complete) { summaryLabel.Text = "Both ends have to be set."; return; }
         if (isImpossibleDay()) { summaryLabel.Text = "That day does not exist in that month."; return; }
         summaryLabel.Text = byDate
            ? "Shows from " + describeDate(fromFirstBox, fromSecondBox) + " to " +
              describeDate(untilFirstBox, untilSecondBox) + ", every year."
            : "Shows from " + fromFirstBox.SelectedItem + " to " + untilFirstBox.SelectedItem + ", every day.";
      }

      private bool isSet(ComboBox firstBox, ComboBox secondBox)
      {
         return firstBox.SelectedIndex >= 0 && (!byDate || secondBox.SelectedIndex >= 0);
      }

      // 31 April is offered by two independent lists but is not a real day
      private bool isImpossibleDay()
      {
         if (!byDate) return false;
         return tooManyDays(fromFirstBox, fromSecondBox) || tooManyDays(untilFirstBox, untilSecondBox);
      }

      private static bool tooManyDays(ComboBox dayBox, ComboBox monthBox)
      {
         if (dayBox.SelectedIndex < 0 || monthBox.SelectedIndex < 0) return false;
         return dayBox.SelectedIndex + 1 > MonthLengths[monthBox.SelectedIndex];
      }

      private static string describeDate(ComboBox dayBox, ComboBox monthBox)
      {
         return (dayBox.SelectedIndex + 1) + " " + MonthNames[monthBox.SelectedIndex];
      }

      private void onAccept(object sender, RoutedEventArgs e)
      {
         From = read(fromFirstBox, fromSecondBox);
         Until = read(untilFirstBox, untilSecondBox);
         DialogResult = true;
      }

      private void onCancel(object sender, RoutedEventArgs e) { DialogResult = false; }
   }
}
