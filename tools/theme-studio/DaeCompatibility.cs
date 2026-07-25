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

      // up-axis: the compiler wants Y-up. rather than rotate every vertex, the whole scene is hung
      // under one node that turns it upright -- the compiler bakes that node into the geometry, so
      // the result is genuinely Y-up. Z-up turns back a quarter about X, X-up a quarter about Z.
      private static bool turnUpright(XmlDocument document, XmlNamespaceManager names)
      {
         XmlElement upAxis = (XmlElement)document.SelectSingleNode("//c:asset/c:up_axis", names);
         string was = upAxis == null ? "Y_UP" : upAxis.InnerText.Trim();
         if (was == "Y_UP") return false;
         string turn = was == "X_UP" ? "0 0 1 90" : "1 0 0 -90";

         foreach (XmlElement scene in document.SelectNodes("//c:library_visual_scenes/c:visual_scene", names)) {
            XmlElement wrapper = element(document, "node");
            wrapper.SetAttribute("id", "yUpCorrection");
            XmlElement rotate = element(document, "rotate");
            rotate.SetAttribute("sid", "rotateX");
            rotate.InnerText = turn;
            wrapper.AppendChild(rotate);

            foreach (XmlNode child in list(scene.ChildNodes))
               if (child.LocalName == "node") wrapper.AppendChild(child);
            scene.AppendChild(wrapper);
         }
         upAxis.InnerText = "Y_UP";
         return true;
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

      // a fixed snapshot, so children can be reparented while iterating
      private static System.Collections.Generic.List<XmlNode> list(XmlNodeList nodes)
      {
         var copy = new System.Collections.Generic.List<XmlNode>();
         foreach (XmlNode node in nodes) copy.Add(node);
         return copy;
      }
   }
}
