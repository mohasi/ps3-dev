using System;
using System.Collections;
using System.Collections.Generic;

namespace RenpyToPs3.RenPy
{
    // Typed, read-only view over a captured Ren'Py AST node (a PyObject).
    // Node base serializes as a tuple state (None, {attrs}); PyCode as (flags, source, ...).
    // Attribute names follow renpy/ast.py. PyExpr (a str subclass) is captured as a PyObject
    // whose first arg is the source text; AsText normalizes both.
    public sealed class AstNode
    {
        public readonly PyObject Obj;

        public AstNode(PyObject obj) { Obj = obj; }

        // Class name without the renpy.ast. prefix (e.g. "Say", "Jump").
        public string Short
        {
            get
            {
                string c = Obj.ClassName;
                return c.StartsWith("renpy.ast.", StringComparison.Ordinal) ? c.Substring("renpy.ast.".Length) : c;
            }
        }

        private IDictionary Attrs
        {
            get
            {
                object[] t = Obj.State as object[];
                if (t != null && t.Length >= 2) return t[1] as IDictionary;
                return null;
            }
        }

        public object Raw(string key)
        {
            IDictionary a = Attrs;
            return (a != null && a.Contains(key)) ? a[key] : null;
        }

        public string Text(string key) { return AsText(Raw(key)); }

        // Normalizes a value to text: plain string, or the source of a PyExpr.
        public static string AsText(object v)
        {
            if (v == null) return null;
            string s = v as string;
            if (s != null) return s;
            PyObject p = v as PyObject;
            if (p != null && p.ClassName.EndsWith(".PyExpr", StringComparison.Ordinal))
            {
                if (p.Args != null && p.Args.Length > 0 && p.Args[0] is string) return (string)p.Args[0];
                object[] st = p.State as object[];
                if (st != null && st.Length > 0 && st[0] is string) return (string)st[0];
            }
            return null;
        }

        public AstNode NodeAt(string key)
        {
            PyObject p = Raw(key) as PyObject;
            return p != null ? new AstNode(p) : null;
        }

        public List<AstNode> Block(string key) { return ToNodes(Raw(key)); }

        public static List<AstNode> ToNodes(object listish)
        {
            List<AstNode> r = new List<AstNode>();
            IList l = listish as IList;
            if (l != null)
                foreach (object it in l)
                {
                    PyObject p = it as PyObject;
                    if (p != null) r.Add(new AstNode(p));
                }
            return r;
        }

        // For Python/PyCode nodes: the embedded Python source string.
        public string PyCodeSource()
        {
            object[] t = Obj.State as object[];
            if (t != null && t.Length > 1 && t[1] is string) return (string)t[1];
            AstNode code = NodeAt("code");
            if (code != null) return code.PyCodeSource();
            return null;
        }

        // The `atl` attribute of a Show/Image/Scene statement (a renpy.atl.RawBlock), or null.
        public PyObject AtlObj() { return Raw("atl") as PyObject; }

        // Image-statement name. Unlike a scene/show imspec (where element [0] is the name
        // tuple), an Image node's "imgname" IS the name tuple directly, e.g. ('bg','alley').
        public string ImageName()
        {
            IList parts = Raw("imgname") as IList;
            if (parts == null) return "";
            System.Text.StringBuilder sb = new System.Text.StringBuilder();
            foreach (object x in parts)
            {
                if (sb.Length > 0) sb.Append(' ');
                sb.Append(x == null ? "" : x.ToString());
            }
            return sb.ToString();
        }

        // The `at` transform list of a scene/show imspec, comma-joined (e.g. "left", "right",
        // "center"). Layout: 7- or 6-tuple -> at_expr_list is element [3]; legacy 3-tuple
        // (name, at_list, layer) -> element [1]. Empty when the statement had no `at` clause.
        public string ImspecAtList() { return ImspecAtList("imspec"); }

        public string ImspecAtList(string key)
        {
            object[] spec = Raw(key) as object[];
            if (spec == null) return "";
            IList at = null;
            if (spec.Length == 3) at = spec[1] as IList;
            else if (spec.Length >= 6) at = spec[3] as IList;
            if (at == null) return "";
            System.Text.StringBuilder sb = new System.Text.StringBuilder();
            foreach (object x in at)
            {
                string s = AsText(x);
                if (s == null) s = (x == null ? "" : x.ToString());
                if (s.Length == 0) continue;
                if (sb.Length > 0) sb.Append(',');
                sb.Append(s);
            }
            return sb.ToString();
        }

        // Image/scene/show name from an imspec tuple (first element is the name parts).
        public string ImspecName() { return ImspecName("imspec"); }

        public string ImspecName(string key)
        {
            object[] spec = Raw(key) as object[];
            if (spec != null && spec.Length > 0)
            {
                IList parts = spec[0] as IList;
                if (parts != null)
                {
                    System.Text.StringBuilder sb = new System.Text.StringBuilder();
                    foreach (object x in parts)
                    {
                        if (sb.Length > 0) sb.Append(' ');
                        sb.Append(x == null ? "" : x.ToString());
                    }
                    return sb.ToString();
                }
            }
            return "";
        }

        // Label name across Ren'Py versions: old "name"; new 8.x "_name" (string) or
        // "name_version"/"name_serial" (tuple form).
        public string LabelName()
        {
            object nm = Raw("name");
            if (nm is string) return (string)nm;
            object _nm = Raw("_name");
            if (_nm is string) return (string)_nm;
            string t = AsText(nm);
            if (t != null) return t;
            object serial = Raw("name_serial");
            if (serial != null)
            {
                object fn = Raw("filename");
                object ver = Raw("name_version");
                return (fn == null ? "?" : fn.ToString()) + ":" + (ver == null ? "0" : ver.ToString()) + ":" + serial;
            }
            return null;
        }
    }
}
