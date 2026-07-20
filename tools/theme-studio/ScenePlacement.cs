using System;

namespace ThemeStudio
{
   // Where a model goes when its own file does not say, and how big it is drawn there.
   //
   // Two problems have to be solved at once. A model built for someone else's scene arrives at
   // their size, so dropped in untouched it either fills the whole screen or is a speck. And the
   // middle and left of the screen belong to the XMB's own menu, so anything left at the origin
   // sits behind the icons where it cannot be seen.
   //
   // So: fitted to a share of the view, and parked in the bottom right corner with a margin. None
   // of this is applied behind the scenes -- it is written into the script as ordinary lines, so
   // it can be read and changed. See PsjsSnippets.MakePlacement.
   //
   // The corner is worked out from where the camera stands and which way it faces, not from the
   // origin, so it is still the bottom right corner in a scene built somewhere far from zero.
   public static class ScenePlacement
   {
      private const double WantedScreenShare = 0.34;   // of the screen's height
      private const double Margin = 0.07;              // kept clear between the model and the edges
      private const double Gap = 0.04;                 // between one model and the next
      private const double Widescreen = 16.0 / 9.0;

      // how much to grow or shrink the model by for it to be drawn at the wanted size
      public static double GetGrowth(SceneProject scene, DaeInfo model)
      {
         if (model.DrawnLargestSide < 0.0001) return 1.0;
         return scene.GetVisibleHeightAt(scene.GetFocusDistance()) * WantedScreenShare / model.DrawnLargestSide;
      }

      // Where to put the model's origin so the shape itself lands in the corner. The origin is not
      // usually the middle of the shape, so the model's own offset is taken back off -- otherwise
      // a shape drawn away from its origin lands somewhere else entirely.
      public static Vec3 GetFreeSpot(SceneProject scene, DaeInfo model, double growth, int slot)
      {
         double distance = scene.GetFocusDistance();
         double height = scene.GetVisibleHeightAt(distance);
         double width = height * Widescreen;
         double size = model.DrawnLargestSide * growth;
         double step = size + height * Gap;

         // fill the bottom row from the right, then start another row above it
         int perRow = Math.Max((int)((width - height * Margin * 2.0) / Math.Max(step, 0.0001)), 1);
         double across = width / 2.0 - height * Margin - size / 2.0 - step * (slot % perRow);
         double up = -height / 2.0 + height * Margin + size / 2.0 + step * (slot / perRow);

         Vec3 forward = SceneProject.normalise(scene.CameraDirection);
         Vec3 upward = SceneProject.normalise(scene.CameraUp);
         Vec3 rightward = cross(forward, upward);

         Vec3 middle = new Vec3(
            scene.CameraPosition.X + forward.X * distance + rightward.X * across + upward.X * up,
            scene.CameraPosition.Y + forward.Y * distance + rightward.Y * across + upward.Y * up,
            scene.CameraPosition.Z + forward.Z * distance + rightward.Z * across + upward.Z * up);

         return new Vec3(middle.X - model.DrawnCenter.X * growth,
                         middle.Y - model.DrawnCenter.Y * growth,
                         middle.Z - model.DrawnCenter.Z * growth);
      }

      private static Vec3 cross(Vec3 first, Vec3 second)
      {
         return new Vec3(first.Y * second.Z - first.Z * second.Y,
                         first.Z * second.X - first.X * second.Z,
                         first.X * second.Y - first.Y * second.X);
      }
   }
}
