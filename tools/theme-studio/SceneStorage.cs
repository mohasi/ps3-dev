
using System.Globalization;
using System.Xml;

namespace ThemeStudio
{
   // saving and loading the 3D scene inside the .themeproj file. kept apart from ThemeProject
   // so the theme's own save/load stays readable.
   public static class SceneStorage
   {
      public static void Write(XmlWriter writer, SceneProject scene)
      {
         writer.WriteStartElement("scene");

         writeVector(writer, "camera", "position", scene.CameraPosition);
         writer.WriteStartElement("cameralens");
         writeNumber(writer, "fov", scene.CameraFieldOfView);
         writeNumber(writer, "near", scene.CameraNear);
         writeNumber(writer, "far", scene.CameraFar);
         writer.WriteAttributeString("direction", scene.CameraDirection.ToString());
         writer.WriteAttributeString("up", scene.CameraUp.ToString());
         writer.WriteEndElement();

         foreach (SceneModel model in scene.Models) {
            writer.WriteStartElement("model");
            writer.WriteAttributeString("id", model.Id);
            writer.WriteAttributeString("file", model.DaePath);
            writer.WriteAttributeString("animation", model.HasAnimation ? "1" : "0");
            writer.WriteEndElement();
         }

         foreach (SceneMaterial material in scene.Materials) {
            writer.WriteStartElement("material");
            writer.WriteAttributeString("id", material.Id);
            writer.WriteAttributeString("effect", material.Effect);
            if (material.TexturePath.Length > 0)
               writer.WriteAttributeString("texture", material.TexturePath);
            writer.WriteEndElement();
         }

         foreach (SceneActor actor in scene.Actors) {
            writer.WriteStartElement("actor");
            writer.WriteAttributeString("id", actor.Id);
            writer.WriteAttributeString("model", actor.ModelId);
            writer.WriteAttributeString("material", actor.MaterialId);
            writer.WriteAttributeString("position", actor.Position.ToString());
            writer.WriteAttributeString("rotation", actor.Rotation.ToString());
            writer.WriteAttributeString("scale", actor.Scale.ToString());
            writer.WriteEndElement();
         }

         foreach (SceneLight light in scene.Lights) {
            writer.WriteStartElement("light");
            writer.WriteAttributeString("id", light.Id);
            writer.WriteAttributeString("type", light.Type);
            writer.WriteAttributeString("color", light.Color.ToString());
            writer.WriteAttributeString("position", light.Position.ToString());
            writer.WriteAttributeString("attenuation", light.Attenuation.ToString());
            writer.WriteEndElement();
         }

         if (scene.ScriptPath.Length > 0) {
            writer.WriteStartElement("script");
            writer.WriteAttributeString("file", scene.ScriptPath);
            writer.WriteEndElement();
         }

         writer.WriteEndElement();
      }

      public static void Read(XmlElement root, SceneProject scene)
      {
         XmlElement element = (XmlElement)root.SelectSingleNode("scene");
         if (element == null) return;

         XmlElement camera = (XmlElement)element.SelectSingleNode("camera");
         if (camera != null) scene.CameraPosition = Vec3.Parse(camera.GetAttribute("position"), scene.CameraPosition);

         XmlElement lens = (XmlElement)element.SelectSingleNode("cameralens");
         if (lens != null) {
            scene.CameraFieldOfView = readNumber(lens, "fov", scene.CameraFieldOfView);
            scene.CameraNear = readNumber(lens, "near", scene.CameraNear);
            scene.CameraFar = readNumber(lens, "far", scene.CameraFar);
            scene.CameraDirection = Vec3.Parse(lens.GetAttribute("direction"), scene.CameraDirection);
            scene.CameraUp = Vec3.Parse(lens.GetAttribute("up"), scene.CameraUp);
         }

         foreach (XmlElement child in element.SelectNodes("model"))
            scene.Models.Add(new SceneModel {
               Id = child.GetAttribute("id"),
               DaePath = child.GetAttribute("file"),
               HasAnimation = child.GetAttribute("animation") == "1"
            });

         foreach (XmlElement child in element.SelectNodes("material"))
            scene.Materials.Add(new SceneMaterial {
               Id = child.GetAttribute("id"),
               Effect = fallback(child.GetAttribute("effect"), SceneEffects.Default),
               TexturePath = child.GetAttribute("texture")
            });

         foreach (XmlElement child in element.SelectNodes("actor"))
            scene.Actors.Add(new SceneActor {
               Id = child.GetAttribute("id"),
               ModelId = child.GetAttribute("model"),
               MaterialId = child.GetAttribute("material"),
               Position = Vec3.Parse(child.GetAttribute("position"), new Vec3(0, 0, 0)),
               Rotation = Vec3.Parse(child.GetAttribute("rotation"), new Vec3(0, 0, 0)),
               Scale = Vec3.Parse(child.GetAttribute("scale"), Vec3.One)
            });

         foreach (XmlElement child in element.SelectNodes("light"))
            scene.Lights.Add(new SceneLight {
               Id = child.GetAttribute("id"),
               Type = fallback(child.GetAttribute("type"), "point"),
               Color = Vec3.Parse(child.GetAttribute("color"), Vec3.One),
               Position = Vec3.Parse(child.GetAttribute("position"), new Vec3(0, 1, 0)),
               Attenuation = Vec3.Parse(child.GetAttribute("attenuation"), new Vec3(0, 1, 4))
            });

         XmlElement script = (XmlElement)element.SelectSingleNode("script");
         if (script != null) scene.ScriptPath = script.GetAttribute("file");
      }

      private static void writeVector(XmlWriter writer, string element, string name, Vec3 value)
      {
         writer.WriteStartElement(element);
         writer.WriteAttributeString(name, value.ToString());
         writer.WriteEndElement();
      }

      private static void writeNumber(XmlWriter writer, string name, double value)
      {
         writer.WriteAttributeString(name, value.ToString("0.######", CultureInfo.InvariantCulture));
      }

      private static double readNumber(XmlElement element, string name, double fallbackValue)
      {
         double value;
         return double.TryParse(element.GetAttribute(name), NumberStyles.Float,
                                CultureInfo.InvariantCulture, out value) ? value : fallbackValue;
      }

      private static string fallback(string value, string whenEmpty)
      {
         return string.IsNullOrEmpty(value) ? whenEmpty : value;
      }
   }
}
