using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Media.Media3D;
using System.Xml;

namespace ThemeStudio
{
   // what the editor needs to know about a COLLADA model before it will compile.
   public class DaeInfo
   {
      public Vec3 Minimum;
      public Vec3 Maximum;
      public int VertexCount;
      public bool HasAnimation;

      // what the model asks for: where it starts, how it is turned, and how big it is drawn.
      // the console uses these as the object's starting values, so anything set on top replaces
      // them rather than adding to them.
      public Vec3 DefaultPosition = new Vec3(0, 0, 0);
      public Vec3 DefaultRotation = new Vec3(0, 0, 0);
      public Vec3 DefaultScale = Vec3.One;
      public bool HasDefaults;
      public bool DefaultsAreExact = true;   // false when the turn was worked out rather than read

      // where the shape really ends up, measured by putting its own placement through every
      // vertex. worked out rather than assumed: a model may be built lying down, turned about an
      // odd axis, or scaled differently on each side, and picking one axis to measure would
      // describe all three wrongly.
      public Vec3 DrawnMinimum;
      public Vec3 DrawnMaximum;

      public Vec3 DrawnSize
      {
         get
         {
            return new Vec3(DrawnMaximum.X - DrawnMinimum.X, DrawnMaximum.Y - DrawnMinimum.Y,
                            DrawnMaximum.Z - DrawnMinimum.Z);
         }
      }

      public Vec3 DrawnCenter
      {
         get
         {
            return new Vec3((DrawnMinimum.X + DrawnMaximum.X) / 2, (DrawnMinimum.Y + DrawnMaximum.Y) / 2,
                            (DrawnMinimum.Z + DrawnMaximum.Z) / 2);
         }
      }

      public double DrawnWidth { get { return DrawnSize.X; } }
      public double DrawnHeight { get { return DrawnSize.Y; } }

      public double DrawnLargestSide
      {
         get { Vec3 size = DrawnSize; return Math.Max(size.X, Math.Max(size.Y, size.Z)); }
      }

      public Vec3 Size { get { return new Vec3(Maximum.X - Minimum.X, Maximum.Y - Minimum.Y, Maximum.Z - Minimum.Z); } }

      // models are often built to sit somewhere inside a larger scene rather than on their own
      // origin, so scaling one alone flings it across the view. this is what to subtract.
      public Vec3 Center
      {
         get
         {
            return new Vec3((Minimum.X + Maximum.X) / 2, (Minimum.Y + Maximum.Y) / 2,
                            (Minimum.Z + Maximum.Z) / 2);
         }
      }

      public double LargestSide
      {
         get { Vec3 size = Size; return Math.Max(size.X, Math.Max(size.Y, size.Z)); }
      }

      // a model authored at a different scale swallows the camera and the whole screen turns
      // one flat colour -- while every compiler still reports success. this is the fit that
      // stops that happening, and it is why an editor is needed at all.
      public double SuggestedScale(double wantedLargestSide)
      {
         double largest = LargestSide;
         return largest > 0 ? wantedLargestSide / largest : 1.0;
      }
   }

   // fills in what each model asks for, so the preview, the object list and the script all start
   // from the same values the console will
   public static class SceneDefaults
   {
      public static void Fill(SceneProject scene, Func<string, string> resolve)
      {
         foreach (SceneModel model in scene.Models) {
            if (model.DrawnSize > 0) continue;   // already known
            DaeInfo info;
            if (!DaeFile.TryRead(resolve(model.DaePath), out info)) continue;
            model.DefaultPosition = info.DefaultPosition;
            model.DefaultRotation = info.DefaultRotation;
            model.DefaultScale = info.DefaultScale;
            model.DrawnSize = info.DrawnLargestSide;
         }
      }

      // where an object starts: its own model's placement, unless the user has moved it
      public static void StartingPlacement(SceneProject scene, SceneActor actor,
                                           out Vec3 position, out Vec3 rotation, out Vec3 scale)
      {
         if (actor.Placed) { position = actor.Position; rotation = actor.Rotation; scale = actor.Scale; return; }
         SceneModel model = scene.FindModel(actor.ModelId);
         if (model == null) { position = new Vec3(0, 0, 0); rotation = new Vec3(0, 0, 0); scale = Vec3.One; return; }
         position = model.DefaultPosition;
         rotation = model.DefaultRotation;
         scale = model.DefaultScale;
      }
   }

   // reads COLLADA (.dae) models: their extent, for the size warning and auto-fit, and their
   // triangles and texture coordinates, for the scene preview.
   public static class DaeFile
   {
      private const string ColladaNamespace = "http://www.collada.org/2005/11/COLLADASchema";

      public static bool TryRead(string path, out DaeInfo info)
      {
         info = null;
         try {
            XmlDocument document;
            XmlNamespaceManager names;
            if (!open(path, out document, out names)) return false;

            var read = new DaeInfo { Minimum = new Vec3(0, 0, 0), Maximum = new Vec3(0, 0, 0) };
            bool any = false;

            // measured where the model actually sits, not where its raw numbers sit: the auto-fit
            // divides by this, and the offset would otherwise be multiplied by the scale it produced
            foreach (Point3D point in getPlacedPositions(document, names)) {
               var placed = new Vec3(point.X, point.Y, point.Z);
               if (!any) { read.Minimum = placed; read.Maximum = placed; any = true; }
               else {
                  read.Minimum = new Vec3(Math.Min(read.Minimum.X, placed.X), Math.Min(read.Minimum.Y, placed.Y),
                                          Math.Min(read.Minimum.Z, placed.Z));
                  read.Maximum = new Vec3(Math.Max(read.Maximum.X, placed.X), Math.Max(read.Maximum.Y, placed.Y),
                                          Math.Max(read.Maximum.Z, placed.Z));
               }
               read.VertexCount++;
            }
            if (!any) return false;
            readDefaultsInto(read, document, names);

            read.HasAnimation = document.SelectSingleNode("//c:library_animations/c:animation", names) != null;
            info = read;
            return true;
         } catch (Exception) {
            return false;
         }
      }

      // builds a single mesh from every triangle in the file, with texture coordinates where the
      // model carries them. good enough to judge placement and surfaces; per-mesh transforms and
      // multiple materials are not applied.
      public static MeshGeometry3D LoadMesh(string path)
      {
         try {
            XmlDocument document;
            XmlNamespaceManager names;
            if (!open(path, out document, out names)) return null;

            var mesh = new MeshGeometry3D();
            List<DaeInstance> instances = DaePlacement.Read(document, names);
            foreach (DaeInstance instance in instances) {
               XmlElement geometry = findGeometry(document, names, instance.GeometryId);
               if (geometry != null) appendGeometry(geometry, names, mesh, instance);
            }

            // a file with no scene graph still has to draw, just without placement
            if (mesh.Positions.Count == 0 && instances.Count == 0)
               foreach (XmlElement geometry in document.SelectNodes("//c:library_geometries/c:geometry", names))
                  appendGeometry(geometry, names, mesh, null);

            if (mesh.Positions.Count == 0) return null;
            addNormals(mesh);

            // the build turns a Z-up (or X-up) model upright for the console, so the preview must do
            // the same or it shows a different orientation than the console draws -- which is how a
            // flat plane can look face-on here and end up edge-on and invisible there.
            turnUpright(mesh, readUpAxis(document, names));
            return mesh;
         } catch (Exception) {
            return null;
         }
      }

      // matches the console's Y-up requirement: Z-up turns back a quarter about X, X-up about Z
      private static void turnUpright(MeshGeometry3D mesh, string upAxis)
      {
         if (upAxis == "Y_UP") return;
         var correction = Matrix3D.Identity;
         correction.Rotate(upAxis == "X_UP" ? new Quaternion(new Vector3D(0, 0, 1), 90)
                                             : new Quaternion(new Vector3D(1, 0, 0), -90));
         for (int index = 0; index < mesh.Positions.Count; index++)
            mesh.Positions[index] = correction.Transform(mesh.Positions[index]);
         for (int index = 0; index < mesh.Normals.Count; index++)
            mesh.Normals[index] = correction.Transform(mesh.Normals[index]);
      }

      private static string readUpAxis(XmlDocument document, XmlNamespaceManager names)
      {
         XmlNode node = document.SelectSingleNode("//c:asset/c:up_axis", names);
         return node == null ? "Y_UP" : node.InnerText.Trim();
      }

      // geometry

      // the first shape's own placement stands for the model: a .dae holding several pieces that
      // each want a different starting place cannot be described by one object anyway.
      // the drawn extent covers every piece regardless, because it is measured, not assumed.
      private static void readDefaultsInto(DaeInfo info, XmlDocument document, XmlNamespaceManager names)
      {
         List<DaeInstance> instances = DaePlacement.Read(document, names);
         bool tookDefaults = false;
         bool any = false;

         foreach (DaeInstance instance in instances) {
            if (instance.HasDefaults && !tookDefaults) {
               info.DefaultPosition = instance.DefaultPosition;
               info.DefaultRotation = instance.DefaultRotation;
               info.DefaultScale = instance.DefaultScale;
               info.DefaultsAreExact = instance.DefaultsAreExact;
               info.HasDefaults = true;
               tookDefaults = true;
            }

            XmlElement geometry = findGeometry(document, names, instance.GeometryId);
            XmlElement meshElement = geometry == null ? null : (XmlElement)geometry.SelectSingleNode("c:mesh", names);
            if (meshElement == null) continue;

            double[] positions = readPositions(meshElement, names);
            for (int index = 0; index + 2 < positions.Length; index += 3) {
               Point3D drawn = place(instance, new Point3D(positions[index], positions[index + 1],
                                                           positions[index + 2]), index / 3);
               drawn = instance.DefaultTransform.Transform(drawn);
               var point = new Vec3(drawn.X, drawn.Y, drawn.Z);
               if (!any) { info.DrawnMinimum = point; info.DrawnMaximum = point; any = true; continue; }
               info.DrawnMinimum = new Vec3(Math.Min(info.DrawnMinimum.X, point.X),
                                            Math.Min(info.DrawnMinimum.Y, point.Y),
                                            Math.Min(info.DrawnMinimum.Z, point.Z));
               info.DrawnMaximum = new Vec3(Math.Max(info.DrawnMaximum.X, point.X),
                                            Math.Max(info.DrawnMaximum.Y, point.Y),
                                            Math.Max(info.DrawnMaximum.Z, point.Z));
            }
         }
      }

      // every vertex of every shape, in the coordinates the console draws them in
      private static IEnumerable<Point3D> getPlacedPositions(XmlDocument document, XmlNamespaceManager names)
      {
         List<DaeInstance> instances = DaePlacement.Read(document, names);
         if (instances.Count == 0) {
            foreach (XmlElement source in document.SelectNodes("//c:mesh/c:source", names)) {
               if (!isPositionSource(source)) continue;
               double[] loose = readFloats(source, names);
               for (int index = 0; index + 2 < loose.Length; index += 3)
                  yield return new Point3D(loose[index], loose[index + 1], loose[index + 2]);
            }
            yield break;
         }

         foreach (DaeInstance instance in instances) {
            XmlElement geometry = findGeometry(document, names, instance.GeometryId);
            XmlElement meshElement = geometry == null ? null : (XmlElement)geometry.SelectSingleNode("c:mesh", names);
            if (meshElement == null) continue;

            double[] positions = readPositions(meshElement, names);
            for (int index = 0; index + 2 < positions.Length; index += 3)
               yield return place(instance, new Point3D(positions[index], positions[index + 1], positions[index + 2]),
                                  index / 3);
         }
      }

      // A shape is drawn exactly as its points were authored.
      //
      // It is tempting to run the skeleton over it first -- a skinned model carries a bind shape
      // and joint transforms that look like they belong. They do not, unless the model also
      // declares an animation to drive that skeleton. Checked on the console: a model with no
      // animation draws at its authored size, and applying the skeleton made it seven times too
      // big. The bind shape does reach the console (folded into a .edge.invbind file) but is only
      // used once something poses the skeleton.
      //
      // So a baked animation is the one thing the preview cannot show. Everything else it draws
      // is what the console draws.
      private static Point3D place(DaeInstance instance, Point3D point, int vertex)
      {
         return point;
      }

      private static XmlElement findGeometry(XmlDocument document, XmlNamespaceManager names, string id)
      {
         foreach (XmlElement geometry in document.SelectNodes("//c:library_geometries/c:geometry", names))
            if (geometry.GetAttribute("id") == id) return geometry;
         return null;
      }

      private static void appendGeometry(XmlElement geometry, XmlNamespaceManager names, MeshGeometry3D mesh,
                                         DaeInstance instance)
      {
         XmlElement meshElement = (XmlElement)geometry.SelectSingleNode("c:mesh", names);
         if (meshElement == null) return;

         double[] positions = readPositions(meshElement, names);
         if (positions.Length == 0) return;

         foreach (XmlElement primitive in meshElement.ChildNodes) {
            if (primitive == null) continue;
            if (primitive.LocalName != "triangles" && primitive.LocalName != "polylist") continue;

            // positions and texture coordinates are indexed separately, so each corner becomes
            // its own vertex rather than trying to share them
            int stride, positionSlot, textureSlot;
            if (!readLayout(primitive, names, out stride, out positionSlot, out textureSlot)) continue;
            double[] textures = readTextureCoordinates(meshElement, primitive, names);
            int[] indices = readInts(primitive, names);

            foreach (int[] corners in getTriangles(primitive, names, indices, stride)) {
               foreach (int corner in corners) {
                  int positionIndex = indices[corner * stride + positionSlot];
                  appendVertex(mesh, positions, positionIndex, instance);

                  if (textureSlot >= 0 && textures.Length > 0) {
                     int textureIndex = indices[corner * stride + textureSlot];
                     appendTextureCoordinate(mesh, textures, textureIndex);
                  }
                  mesh.TriangleIndices.Add(mesh.Positions.Count - 1);
               }
            }
         }
      }

      private static void appendVertex(MeshGeometry3D mesh, double[] positions, int index, DaeInstance instance)
      {
         int at = index * 3;
         if (at + 2 >= positions.Length) { mesh.Positions.Add(new Point3D()); return; }
         mesh.Positions.Add(place(instance, new Point3D(positions[at], positions[at + 1], positions[at + 2]), index));
      }

      // collada measures the vertical texture axis from the bottom, wpf from the top
      private static void appendTextureCoordinate(MeshGeometry3D mesh, double[] textures, int index)
      {
         int at = index * 2;
         if (at + 1 < textures.Length) mesh.TextureCoordinates.Add(new Point(textures[at], 1 - textures[at + 1]));
         else mesh.TextureCoordinates.Add(new Point());
      }

      // yields each triangle as three corner numbers, fanning larger polygons from their first corner
      private static IEnumerable<int[]> getTriangles(XmlElement primitive, XmlNamespaceManager names,
                                                     int[] indices, int stride)
      {
         int cornersAvailable = stride > 0 ? indices.Length / stride : 0;

         XmlElement counts = (XmlElement)primitive.SelectSingleNode("c:vcount", names);
         if (counts == null) {
            for (int corner = 0; corner + 2 < cornersAvailable; corner += 3)
               yield return new[] { corner, corner + 1, corner + 2 };
            yield break;
         }

         int cursor = 0;
         foreach (int sides in parseInts(counts.InnerText)) {
            for (int corner = 1; corner + 1 < sides; corner++) {
               if (cursor + corner + 1 >= cornersAvailable) yield break;
               yield return new[] { cursor, cursor + corner, cursor + corner + 1 };
            }
            cursor += sides;
         }
      }

      // which slot of each index group holds the position, and which the texture coordinate
      private static bool readLayout(XmlElement primitive, XmlNamespaceManager names,
                                     out int stride, out int positionSlot, out int textureSlot)
      {
         stride = 0;
         positionSlot = -1;
         textureSlot = -1;
         foreach (XmlElement input in primitive.SelectNodes("c:input", names)) {
            int slot;
            if (!int.TryParse(input.GetAttribute("offset"), out slot)) slot = 0;
            stride = Math.Max(stride, slot + 1);
            string semantic = input.GetAttribute("semantic");
            if (semantic == "VERTEX") positionSlot = slot;
            else if (semantic == "TEXCOORD" && textureSlot < 0) textureSlot = slot;
         }
         return positionSlot >= 0 && stride > 0;
      }

      private static double[] readPositions(XmlElement mesh, XmlNamespaceManager names)
      {
         foreach (XmlElement source in mesh.SelectNodes("c:source", names))
            if (isPositionSource(source)) return readFloats(source, names);
         return new double[0];
      }

      // follows the primitive's TEXCOORD input back to the source it names
      private static double[] readTextureCoordinates(XmlElement mesh, XmlElement primitive,
                                                     XmlNamespaceManager names)
      {
         foreach (XmlElement input in primitive.SelectNodes("c:input", names)) {
            if (input.GetAttribute("semantic") != "TEXCOORD") continue;
            string sourceId = input.GetAttribute("source").TrimStart('#');
            foreach (XmlElement source in mesh.SelectNodes("c:source", names))
               if (source.GetAttribute("id") == sourceId) return readFloats(source, names);
         }
         return new double[0];
      }

      // sources are named by convention (…-positions / …-POSITION)
      private static bool isPositionSource(XmlElement source)
      {
         string id = source.GetAttribute("id");
         if (id.IndexOf("position", StringComparison.OrdinalIgnoreCase) >= 0) return true;
         return id.IndexOf("POSITION", StringComparison.Ordinal) >= 0;
      }

      // parsing

      private static bool open(string path, out XmlDocument document, out XmlNamespaceManager names)
      {
         document = new XmlDocument();
         document.Load(path);
         names = new XmlNamespaceManager(document.NameTable);
         names.AddNamespace("c", ColladaNamespace);
         return document.DocumentElement != null;
      }

      private static double[] readFloats(XmlElement source, XmlNamespaceManager names)
      {
         XmlElement array = (XmlElement)source.SelectSingleNode("c:float_array", names);
         if (array == null) return new double[0];

         string[] parts = array.InnerText.Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
         var values = new double[parts.Length];
         for (int index = 0; index < parts.Length; index++)
            double.TryParse(parts[index], NumberStyles.Float, CultureInfo.InvariantCulture, out values[index]);
         return values;
      }

      private static int[] readInts(XmlElement primitive, XmlNamespaceManager names)
      {
         XmlElement indices = (XmlElement)primitive.SelectSingleNode("c:p", names);
         return indices == null ? new int[0] : parseInts(indices.InnerText);
      }

      private static int[] parseInts(string text)
      {
         string[] parts = text.Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
         var values = new int[parts.Length];
         for (int index = 0; index < parts.Length; index++)
            int.TryParse(parts[index], out values[index]);
         return values;
      }

      // wpf shades a mesh from its normals; collada indexes its own separately, so averaging
      // face normals here is both simpler and safe.
      private static void addNormals(MeshGeometry3D mesh)
      {
         var normals = new Vector3D[mesh.Positions.Count];
         for (int index = 0; index + 2 < mesh.TriangleIndices.Count; index += 3) {
            int a = mesh.TriangleIndices[index], b = mesh.TriangleIndices[index + 1], c = mesh.TriangleIndices[index + 2];
            if (a >= normals.Length || b >= normals.Length || c >= normals.Length) continue;
            Vector3D face = Vector3D.CrossProduct(mesh.Positions[b] - mesh.Positions[a],
                                                  mesh.Positions[c] - mesh.Positions[a]);
            normals[a] += face; normals[b] += face; normals[c] += face;
         }
         foreach (Vector3D normal in normals) {
            Vector3D unit = normal;
            if (unit.Length > 0) unit.Normalize(); else unit = new Vector3D(0, 0, 1);
            mesh.Normals.Add(unit);
         }
      }
   }
}
