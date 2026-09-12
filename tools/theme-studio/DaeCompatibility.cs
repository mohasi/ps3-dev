using System;
using System.Xml;

namespace ThemeStudio
{
   // Blender and several other editors export COLLADA that Sony's raf_compiler refuses to build:
   // the model is Z-up (the compiler only accepts Y-up), and the mesh names no material (the
   // compiler needs one to hang the texture on). Both are mechanical to fix, so rather than send
   // the user back to re-export, a compiler-ready copy is written on the way to the build. The
   // user's own file is never touched.
   public static class DaeCompatibility
   {
      private const string Ns = "http://www.collada.org/2005/11/COLLADASchema";
      private const string MaterialSymbol = "themeStudioSurface";
      private const string MaterialId = "themeStudioMaterial";
      private const string EffectId = "themeStudioEffect";

      public class Changes
      {
         public bool TurnedUpright;   // was Z-up or X-up, now Y-up
         public bool BoundMaterial;   // had no material for the compiler to attach the texture to
         public bool AnyMade { get { return TurnedUpright || BoundMaterial; } }
      }

      // writes a compiler-ready copy of a .dae and reports what had to be changed. a file that
      // already builds passes through unchanged.
      public static Changes WriteReady(string sourcePath, string destPath)
      {
         var changes = new Changes();
         var document = new XmlDocument();
         document.Load(sourcePath);
         var names = new XmlNamespaceManager(document.NameTable);
         names.AddNamespace("c", Ns);

         changes.TurnedUpright = turnUpright(document, names);
         changes.BoundMaterial = bindMaterial(document, names);
         document.Save(destPath);
         return changes;
      }

      // up-axis: the console wants Y-up, so the vertices themselves are turned.
      //
      // this used to hang the scene under one node that rotated it, which turned nothing at all:
      // raf_geom reads neither node transforms nor up_axis, it bakes the raw arrays and ignores
      // the rest of the file. converting one model four ways -- Z_UP, the same file claiming Y_UP,
      // with the rotating node and with that node's rotation removed -- gave four byte-identical
      // .edge files (AA7065A5EA49B0CD04B358668723F803, 12 September 2026). so every Blender export
      // reached the console lying on its side while the preview stood it up, and the two disagreed
      // about every model a user imported.
      //
      // the turn matches the one the preview applies in DaeFile.turnUpright, so they now agree.
      private static bool turnUpright(XmlDocument document, XmlNamespaceManager names)
      {
         XmlElement upAxis = (XmlElement)document.SelectSingleNode("//c:asset/c:up_axis", names);
         string was = upAxis == null ? "Y_UP" : upAxis.InnerText.Trim();
         if (was == "Y_UP") return false;

         foreach (string sourceId in getVertexSourceIds(document, names))
            turnFloatArray(document, names, sourceId, was);

         upAxis.InnerText = "Y_UP";
         return true;
      }

      // the ids of the sources holding positions and normals. texture coordinates must be left
      // alone, so the arrays are found through the inputs that name them rather than by taking
      // every source in the mesh.
      private static System.Collections.Generic.List<string> getVertexSourceIds(XmlDocument document,
                                                                               XmlNamespaceManager names)
      {
         var ids = new System.Collections.Generic.List<string>();
         foreach (XmlElement input in document.SelectNodes(
                     "//c:library_geometries//c:input[@semantic='POSITION' or @semantic='NORMAL']", names)) {
            string id = input.GetAttribute("source").TrimStart('#');
            if (id.Length > 0 && !ids.Contains(id)) ids.Add(id);
         }
         return ids;
      }

      // Z-up turns back a quarter about X, so x stays and (y, z) becomes (z, -y).
      // X-up turns a quarter about Z, so z stays and (x, y) becomes (-y, x).
      private static void turnFloatArray(XmlDocument document, XmlNamespaceManager names,
                                         string sourceId, string was)
      {
         XmlElement array = (XmlElement)document.SelectSingleNode(
            "//c:source[@id='" + sourceId + "']/c:float_array", names);
         if (array == null) return;

         string[] parts = array.InnerText.Split(new[] { ' ', '\t', '\r', '\n' },
                                                StringSplitOptions.RemoveEmptyEntries);
         if (parts.Length % 3 != 0) return;   // not triples, so not something to turn

         var turned = new System.Text.StringBuilder(array.InnerText.Length);
         for (int at = 0; at < parts.Length; at += 3) {
            double x = parseNumber(parts[at]), y = parseNumber(parts[at + 1]), z = parseNumber(parts[at + 2]);
            double newX = was == "X_UP" ? -y : x;
            double newY = was == "X_UP" ? x : z;
            double newZ = was == "X_UP" ? z : -y;

            if (at > 0) turned.Append(' ');
            turned.Append(writeNumber(newX)).Append(' ').Append(writeNumber(newY)).Append(' ').Append(writeNumber(newZ));
         }
         array.InnerText = turned.ToString();
      }

      private static double parseNumber(string text)
      {
         double value;
         double.TryParse(text, System.Globalization.NumberStyles.Float,
                         System.Globalization.CultureInfo.InvariantCulture, out value);
         return value;
      }

      // plain decimals, never exponents. round-tripping a double writes very small numbers as
      // "5.34E-06", and nothing says the compiler's parser reads that; six places is finer than the
      // floats the geometry ends up in anyway.
      private static string writeNumber(double value)
      {
         return value.ToString("0.######", System.Globalization.CultureInfo.InvariantCulture);
      }

      // material: the compiler needs every shape to name a material, which is where it reads the
      // texture-coordinate binding from. Blender leaves this out, so a plain white one is added and
      // pointed at -- the theme's real effect and texture come from the scene, not from here.
      private static bool bindMaterial(XmlDocument document, XmlNamespaceManager names)
      {
         var toBind = new System.Collections.Generic.List<XmlElement>();
         foreach (XmlElement instance in document.SelectNodes("//c:instance_geometry", names))
            if (instance.SelectSingleNode("c:bind_material", names) == null) toBind.Add(instance);
         if (toBind.Count == 0) return false;

         ensureMaterialLibraries(document, names);
         foreach (XmlElement instance in toBind)
            bindOne(document, names, instance);
         return true;
      }

      private static void ensureMaterialLibraries(XmlDocument document, XmlNamespaceManager names)
      {
         XmlElement root = document.DocumentElement;

         if (document.SelectSingleNode("//c:library_effects", names) == null) {
            XmlElement library = element(document, "library_effects");
            XmlElement effect = element(document, "effect");
            effect.SetAttribute("id", EffectId);
            XmlElement profile = element(document, "profile_COMMON");
            XmlElement technique = element(document, "technique");
            technique.SetAttribute("sid", "common");
            XmlElement lambert = element(document, "lambert");
            XmlElement diffuse = element(document, "diffuse");
            XmlElement colour = element(document, "color");
            colour.InnerText = "1 1 1 1";
            diffuse.AppendChild(colour);
            lambert.AppendChild(diffuse);
            technique.AppendChild(lambert);
            profile.AppendChild(technique);
            effect.AppendChild(profile);
            library.AppendChild(effect);
            root.AppendChild(library);
         }

         if (document.SelectSingleNode("//c:library_materials", names) == null) {
            XmlElement library = element(document, "library_materials");
            XmlElement material = element(document, "material");
            material.SetAttribute("id", MaterialId);
            material.SetAttribute("name", MaterialId);
            XmlElement instanceEffect = element(document, "instance_effect");
            instanceEffect.SetAttribute("url", "#" + EffectId);
            material.AppendChild(instanceEffect);
            library.AppendChild(material);
            root.AppendChild(library);
         }
      }

      // names the material on the shape's triangles, then binds it on the instance. if the shape
      // carries texture coordinates, the binding says which set feeds the texture.
      private static void bindOne(XmlDocument document, XmlNamespaceManager names, XmlElement instance)
      {
         string geometryId = instance.GetAttribute("url").TrimStart('#');
         bool hasTexture = false;

         foreach (XmlElement geometry in document.SelectNodes("//c:library_geometries/c:geometry", names)) {
            if (geometry.GetAttribute("id") != geometryId) continue;
            foreach (XmlElement primitive in geometry.SelectNodes("c:mesh/c:triangles | c:mesh/c:polylist", names)) {
               primitive.SetAttribute("material", MaterialSymbol);
               if (primitive.SelectSingleNode("c:input[@semantic='TEXCOORD']", names) != null) hasTexture = true;
            }
         }

         XmlElement bind = element(document, "bind_material");
         XmlElement common = element(document, "technique_common");
         XmlElement instanceMaterial = element(document, "instance_material");
         instanceMaterial.SetAttribute("symbol", MaterialSymbol);
         instanceMaterial.SetAttribute("target", "#" + MaterialId);
         if (hasTexture) {
            XmlElement vertexInput = element(document, "bind_vertex_input");
            vertexInput.SetAttribute("semantic", "TEX0");
            vertexInput.SetAttribute("input_semantic", "TEXCOORD");
            vertexInput.SetAttribute("input_set", "0");
            instanceMaterial.AppendChild(vertexInput);
         }
         common.AppendChild(instanceMaterial);
         bind.AppendChild(common);
         instance.AppendChild(bind);
      }

      private static XmlElement element(XmlDocument document, string name)
      {
         return document.CreateElement(name, Ns);
      }

   }
}
