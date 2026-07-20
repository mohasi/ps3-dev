using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows.Media.Media3D;
using System.Xml;

namespace ThemeStudio
{
   // where a model's geometry actually sits. a .dae rarely stores its shape at the origin: the
   // shape is authored wherever it belonged in the artist's scene, and a transform in the scene
   // graph puts it there. Sony's own theme models are offset by about 15 units on x, which is why
   // their camera sits at x=15.5 and their actors carry no position at all.
   //
   // reading the raw vertex list and ignoring this is the mistake that made the editor and the
   // console disagree: the preview looked right, the console put half the scene off screen.
   public class DaeInstance
   {
      public string GeometryId = "";
      public DaeSkin Skin;   // set when the shape is skinned; its skeleton moves the vertices

      // What the model asks to be done to it. The compiler lifts this out of the .dae and makes
      // it the object's STARTING position, rotation and size -- it is not baked into the shape.
      // So anything the editor or a script sets REPLACES it rather than adding to it, which is
      // the whole reason a background sheet can end up a flat sliver.
      public Vec3 DefaultPosition = new Vec3(0, 0, 0);
      public Vec3 DefaultRotation = new Vec3(0, 0, 0);   // radians
      public Vec3 DefaultScale = Vec3.One;
      public bool HasDefaults;

      // the same placement as one transform, used to measure where the shape really ends up.
      // measuring from this rather than from a chosen axis is what keeps a model that was built
      // sideways, or turned about an odd axis, from being described wrongly.
      public Matrix3D DefaultTransform = Matrix3D.Identity;

      // false when the file turns the model about an axis that is not simply x, y or z. the
      // three numbers above are then an approximation, and anything shown to the user says so.
      public bool DefaultsAreExact = true;
   }

   // A skinned shape is placed by its skeleton: each vertex ends up at the weighted sum of
   // (set up the shape -> undo the bind pose -> the joint's place).
   //
   // The bind shape matrix IS part of that, and this was got wrong once by testing too little.
   // Changing it leaves the geometry and skeleton files byte-identical, which looks like it is
   // ignored -- but the compiler folds it into the inverse bind matrix it ships alongside them
   // (`.edge.invbind`, big-endian, and exactly invBind x bindShape). The compiled .raf does
   // change. Compare final output, not the first intermediate that comes to hand.
   public class DaeSkin
   {
      public Matrix3D BindShape = Matrix3D.Identity;
      public readonly List<Matrix3D> JointTransforms = new List<Matrix3D>();   // one per joint, already combined
      public readonly List<int[]> BoneOfVertex = new List<int[]>();
      public readonly List<double[]> WeightOfVertex = new List<double[]>();

      public Point3D Place(Point3D point)
      {
         return Place(point, -1);
      }

      public Point3D Place(Point3D point, int vertex)
      {
         Point3D bound = BindShape.Transform(point);
         if (vertex < 0 || vertex >= BoneOfVertex.Count || JointTransforms.Count == 0)
            return JointTransforms.Count > 0 ? JointTransforms[0].Transform(bound) : bound;

         int[] bones = BoneOfVertex[vertex];
         double[] weights = WeightOfVertex[vertex];
         if (bones.Length == 0) return JointTransforms[0].Transform(bound);

         double x = 0, y = 0, z = 0, total = 0;
         for (int index = 0; index < bones.Length; index++) {
            if (bones[index] < 0 || bones[index] >= JointTransforms.Count) continue;
            Point3D moved = JointTransforms[bones[index]].Transform(bound);
            double weight = weights[index];
            x += moved.X * weight; y += moved.Y * weight; z += moved.Z * weight;
            total += weight;
         }
         if (total <= 0) return JointTransforms[0].Transform(bound);
         return new Point3D(x / total, y / total, z / total);
      }
   }

   // reads the scene graph of a .dae and works out where each piece of geometry ends up
   public static class DaePlacement
   {
      public static List<DaeInstance> Read(XmlDocument document, XmlNamespaceManager names)
      {
         var instances = new List<DaeInstance>();
         var jointWorldBySid = new Dictionary<string, Matrix3D>();
         var jointWorldById = new Dictionary<string, Matrix3D>();

         foreach (XmlElement scene in document.SelectNodes("//c:library_visual_scenes/c:visual_scene", names))
            walk(scene, names, Matrix3D.Identity, instances, jointWorldBySid, jointWorldById);

         foreach (DaeInstance instance in instances)
            if (instance.Skin != null) fillJoints(instance, document, names, jointWorldBySid, jointWorldById);

         return instances;
      }

      // depth first, carrying each node's place in the world down to its children
      private static void walk(XmlElement node, XmlNamespaceManager names, Matrix3D parentWorld,
                               List<DaeInstance> instances, Dictionary<string, Matrix3D> jointWorldBySid,
                               Dictionary<string, Matrix3D> jointWorldById)
      {
         foreach (XmlNode child in node.ChildNodes) {
            var element = child as XmlElement;
            if (element == null || element.LocalName != "node") continue;

            Matrix3D world = Matrix3D.Multiply(readLocalTransform(element, names), parentWorld);

            string sid = element.GetAttribute("sid");
            string id = element.GetAttribute("id");
            if (sid.Length > 0) jointWorldBySid[sid] = world;
            if (id.Length > 0) jointWorldById[id] = world;

            // a plain shape's transform becomes its starting placement, exactly as the compiler
            // does it -- the shape itself stays as the artist drew it
            foreach (XmlElement geometry in element.SelectNodes("c:instance_geometry", names)) {
               var instance = new DaeInstance {
                  GeometryId = geometry.GetAttribute("url").TrimStart('#'), DefaultTransform = world
               };
               readDefaults(element, names, instance);
               instances.Add(instance);
            }

            foreach (XmlElement controller in element.SelectNodes("c:instance_controller", names))
               instances.Add(new DaeInstance {
                  GeometryId = controller.GetAttribute("url").TrimStart('#'), Skin = new DaeSkin()
               });

            walk(element, names, world, instances, jointWorldBySid, jointWorldById);
         }
      }

      // the node's own translate / rotate / scale, read as written rather than worked back out of
      // a combined matrix -- the signs matter, and a negative scale is a real thing artists do
      private static void readDefaults(XmlElement node, XmlNamespaceManager names, DaeInstance instance)
      {
         foreach (XmlNode child in node.ChildNodes) {
            var element = child as XmlElement;
            if (element == null) continue;
            double[] values = readFloats(element.InnerText);

            if (element.LocalName == "translate" && values.Length >= 3) {
               instance.DefaultPosition = new Vec3(values[0], values[1], values[2]);
               instance.HasDefaults = true;
            } else if (element.LocalName == "scale" && values.Length >= 3) {
               instance.DefaultScale = new Vec3(values[0], values[1], values[2]);
               instance.HasDefaults = true;
            } else if (element.LocalName == "rotate" && values.Length >= 4 && values[3] != 0) {
               addTurn(instance, values);
               instance.HasDefaults = true;
            } else if (element.LocalName == "matrix") {
               // one combined transform rather than the usual three: nothing to read off, so the
               // parts are worked back out of it and flagged as worked out rather than read
               instance.DefaultsAreExact = false;
               instance.HasDefaults = true;
            }
         }
      }

      // a turn is written as an axis and an angle. exporters use one plain axis at a time, which
      // maps straight onto a part of the turn; anything else is kept in the transform and marked
      // as not readable off, rather than being quietly filed under the nearest axis.
      private static void addTurn(DaeInstance instance, double[] values)
      {
         double radians = values[3] * Math.PI / 180.0;
         double x = Math.Abs(values[0]), y = Math.Abs(values[1]), z = Math.Abs(values[2]);
         const double Sideways = 0.001;   // how much of another axis still counts as "none"

         if (x > 0.999 && y < Sideways && z < Sideways) instance.DefaultRotation.X += radians * Math.Sign(values[0]);
         else if (y > 0.999 && x < Sideways && z < Sideways) instance.DefaultRotation.Y += radians * Math.Sign(values[1]);
         else if (z > 0.999 && x < Sideways && y < Sideways) instance.DefaultRotation.Z += radians * Math.Sign(values[2]);
         else instance.DefaultsAreExact = false;
      }

      // translate / rotate / scale / matrix, applied in the order they are written
      private static Matrix3D readLocalTransform(XmlElement node, XmlNamespaceManager names)
      {
         Matrix3D local = Matrix3D.Identity;
         foreach (XmlNode child in node.ChildNodes) {
            var element = child as XmlElement;
            if (element == null) continue;

            double[] values = readFloats(element.InnerText);
            Matrix3D step;
            switch (element.LocalName) {
               case "translate":
                  if (values.Length < 3) continue;
                  step = Matrix3D.Identity;
                  step.OffsetX = values[0]; step.OffsetY = values[1]; step.OffsetZ = values[2];
                  break;
               case "rotate":
                  if (values.Length < 4 || values[3] == 0) continue;
                  step = Matrix3D.Identity;
                  step.Rotate(new Quaternion(new Vector3D(values[0], values[1], values[2]), values[3]));
                  break;
               case "scale":
                  if (values.Length < 3) continue;
                  step = new Matrix3D(values[0], 0, 0, 0, 0, values[1], 0, 0, 0, 0, values[2], 0, 0, 0, 0, 1);
                  break;
               case "matrix":
                  if (values.Length < 16) continue;
                  step = toMatrix(values, 0);
                  break;
               default:
                  continue;
            }
            // collada applies the list right to left, so each new one wraps the ones before it
            local = Matrix3D.Multiply(step, local);
         }
         return local;
      }

      // a skinned instance names a controller, not a geometry; follow it to the shape it deforms
      // and to the skeleton that places it
      private static void fillJoints(DaeInstance instance, XmlDocument document, XmlNamespaceManager names,
                                     Dictionary<string, Matrix3D> jointWorldBySid,
                                     Dictionary<string, Matrix3D> jointWorldById)
      {
         XmlElement controller = findById(document, names, "//c:library_controllers/c:controller", instance.GeometryId);
         XmlElement skin = controller == null ? null : (XmlElement)controller.SelectSingleNode("c:skin", names);
         if (skin == null) { instance.Skin = null; return; }

         instance.GeometryId = skin.GetAttribute("source").TrimStart('#');

         double[] bindShape = readFloats(text(skin, "c:bind_shape_matrix", names));
         if (bindShape.Length >= 16) instance.Skin.BindShape = toMatrix(bindShape, 0);

         string[] jointNames = readNames(sourceFor(skin, names, "JOINT"));
         double[] inverseBinds = readFloats(sourceText(skin, names, "INV_BIND_MATRIX"));

         for (int index = 0; index < jointNames.Length; index++) {
            Matrix3D jointWorld;
            if (!jointWorldBySid.TryGetValue(jointNames[index], out jointWorld) &&
                !jointWorldById.TryGetValue(jointNames[index], out jointWorld))
               jointWorld = Matrix3D.Identity;

            Matrix3D inverseBind = inverseBinds.Length >= (index + 1) * 16 ? toMatrix(inverseBinds, index * 16)
                                                                           : Matrix3D.Identity;
            instance.Skin.JointTransforms.Add(Matrix3D.Multiply(inverseBind, jointWorld));
         }
         if (instance.Skin.JointTransforms.Count == 0) instance.Skin.JointTransforms.Add(Matrix3D.Identity);

         readWeights(skin, names, instance.Skin);
      }

      private static void readWeights(XmlElement skin, XmlNamespaceManager names, DaeSkin into)
      {
         XmlElement weights = (XmlElement)skin.SelectSingleNode("c:vertex_weights", names);
         if (weights == null) return;

         int stride = 0, jointSlot = -1, weightSlot = -1;
         string weightSource = "";
         foreach (XmlElement input in weights.SelectNodes("c:input", names)) {
            int slot;
            if (!int.TryParse(input.GetAttribute("offset"), out slot)) slot = 0;
            stride = Math.Max(stride, slot + 1);
            if (input.GetAttribute("semantic") == "JOINT") jointSlot = slot;
            else if (input.GetAttribute("semantic") == "WEIGHT") {
               weightSlot = slot;
               weightSource = input.GetAttribute("source").TrimStart('#');
            }
         }
         if (stride == 0 || jointSlot < 0) return;

         double[] weightValues = readFloats(sourceTextById(skin, names, weightSource));
         int[] counts = readInts(text(weights, "c:vcount", names));
         int[] pairs = readInts(text(weights, "c:v", names));

         int cursor = 0;
         foreach (int count in counts) {
            var bones = new int[count];
            var shares = new double[count];
            for (int entry = 0; entry < count; entry++) {
               int at = (cursor + entry) * stride;
               if (at + stride > pairs.Length) break;
               bones[entry] = pairs[at + jointSlot];
               int weightIndex = weightSlot >= 0 ? pairs[at + weightSlot] : -1;
               shares[entry] = weightIndex >= 0 && weightIndex < weightValues.Length ? weightValues[weightIndex] : 1.0;
            }
            into.BoneOfVertex.Add(bones);
            into.WeightOfVertex.Add(shares);
            cursor += count;
         }
      }

      // collada writes a matrix a row at a time and multiplies it onto a column of numbers;
      // wpf does the opposite, so the whole thing is flipped over its diagonal on the way in
      private static Matrix3D toMatrix(double[] values, int at)
      {
         return new Matrix3D(values[at + 0], values[at + 4], values[at + 8], values[at + 12],
                             values[at + 1], values[at + 5], values[at + 9], values[at + 13],
                             values[at + 2], values[at + 6], values[at + 10], values[at + 14],
                             values[at + 3], values[at + 7], values[at + 11], values[at + 15]);
      }

      // finding things

      private static XmlElement findById(XmlDocument document, XmlNamespaceManager names, string path, string id)
      {
         foreach (XmlElement element in document.SelectNodes(path, names))
            if (element.GetAttribute("id") == id) return element;
         return null;
      }

      private static XmlElement sourceFor(XmlElement skin, XmlNamespaceManager names, string semantic)
      {
         foreach (XmlElement input in skin.SelectNodes("c:joints/c:input", names))
            if (input.GetAttribute("semantic") == semantic)
               return findSource(skin, names, input.GetAttribute("source").TrimStart('#'));
         return null;
      }

      private static string sourceText(XmlElement skin, XmlNamespaceManager names, string semantic)
      {
         XmlElement source = sourceFor(skin, names, semantic);
         return source == null ? "" : text(source, "c:float_array", names);
      }

      private static string sourceTextById(XmlElement skin, XmlNamespaceManager names, string id)
      {
         XmlElement source = findSource(skin, names, id);
         return source == null ? "" : text(source, "c:float_array", names);
      }

      private static XmlElement findSource(XmlElement skin, XmlNamespaceManager names, string id)
      {
         foreach (XmlElement source in skin.SelectNodes("c:source", names))
            if (source.GetAttribute("id") == id) return source;
         return null;
      }

      private static string text(XmlElement parent, string path, XmlNamespaceManager names)
      {
         XmlNode found = parent.SelectSingleNode(path, names);
         return found == null ? "" : found.InnerText;
      }

      private static string[] readNames(XmlElement source)
      {
         if (source == null) return new string[0];
         foreach (XmlNode child in source.ChildNodes)
            if (child.LocalName == "Name_array" || child.LocalName == "IDREF_array")
               return child.InnerText.Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
         return new string[0];
      }

      private static double[] readFloats(string text)
      {
         string[] parts = text.Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
         var values = new double[parts.Length];
         for (int index = 0; index < parts.Length; index++)
            double.TryParse(parts[index], NumberStyles.Float, CultureInfo.InvariantCulture, out values[index]);
         return values;
      }

      private static int[] readInts(string text)
      {
         string[] parts = text.Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
         var values = new int[parts.Length];
         for (int index = 0; index < parts.Length; index++) int.TryParse(parts[index], out values[index]);
         return values;
      }
   }
}
