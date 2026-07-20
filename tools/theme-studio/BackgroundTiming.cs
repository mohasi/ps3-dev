using System.Collections.Generic;

namespace ThemeStudio
{
   // when each background picture is on show, in words rather than in the console's own shorthand.
   //
   // the console's rule for anything the schedule misses is simple and generous: if the date or
   // hour falls outside every picture's range, the FIRST picture in the list shows instead. so a
   // gap can never leave the console with no background -- it only means the first picture is
   // doing more work than the person setting it up may realise, which is worth saying out loud.
   public static class BackgroundTiming
   {
      public static readonly string[] MonthNames = {
         "January", "February", "March", "April", "May", "June",
         "July", "August", "September", "October", "November", "December"
      };

      public static string Describe(Background background, bool byDate)
      {
         if (background.From.Length == 0 || background.Until.Length == 0)
            return byDate ? "shows all year" : "shows all day";
         return byDate
            ? describeDate(background.From) + " to " + describeDate(background.Until)
            : describeHour(background.From) + " to " + describeHour(background.Until);
      }

      // what to say about the schedule as a whole, or "" when there is nothing worth saying
      public static string DescribeCoverage(IList<Background> backgrounds, bool byDate)
      {
         int scheduled = 0;
         foreach (Background background in backgrounds)
            if (background.From.Length > 0 && background.Until.Length > 0) scheduled++;

         if (scheduled == 0)
            return "None of these have a time set, so the first one always shows.";
         if (scheduled < backgrounds.Count)
            return "Some of these have no time set. They will never show on their own -- " +
                   "only the first picture stands in for whatever is not covered.";
         return "Anything these do not cover falls back to the first picture, so there is never a " +
                "blank background.";
      }

      private static string describeDate(string stored)
      {
         if (stored.Length == 8) stored = stored.Substring(4);
         if (stored.Length != 4) return stored;
         int month, day;
         if (!int.TryParse(stored.Substring(0, 2), out month) ||
             !int.TryParse(stored.Substring(2, 2), out day) ||
             month < 1 || month > 12) return stored;
         return day + " " + MonthNames[month - 1];
      }

      private static string describeHour(string stored)
      {
         int hour;
         return int.TryParse(stored, out hour) ? hour.ToString("00") + ":00" : stored;
      }
   }
}
