using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;

namespace DebugBridgeClient
{
    // Dense per-event grid that makes anomalies visible at a glance.
    // Each cell is one trace event in capture order. Color encodes
    // RARITY ONLY (no per-Nid hue): the eye is not distracted by
    // identity, only by "how unusual is this call".
    //   common  -> dim gray   (log-scaled, so 24k vs 12k look the same)
    //   medium  -> warm amber
    //   rare    -> bright red
    // Repeated occurrences of the same rare Nid still cluster as same
    // color, so bursts are obvious without painting a rainbow.
    //
    // Cells are baked into a RenderTargetBitmap once per SetEvents();
    // hover/click are handled by a single overlay Rectangle so we never
    // re-render the (potentially 24k-cell) grid on mouse moves.
    public class TraceHeatmap : Grid
    {
        private const int   MaxCellPx     = 8;
        private const int   HoverScale    = 3;
        private const int   HoverAnimMs   = 120;


        private readonly Image image;
        private readonly Canvas overlay;
        private readonly Rectangle hoverRect;
        private readonly ScaleTransform hoverScale;

        private int cellPx = MaxCellPx;
        private int cols;
        private int rowCount;
        private int eventCount;
        private uint[] nids = new uint[0];
        private Color[] colors = new Color[0];
        private string[] tooltips = new string[0];
        private int hoveredIndex = -1;

        public event Action<int> CellClicked;

        public TraceHeatmap()
        {
            Background = Brushes.Transparent;
            ClipToBounds = true;
            HorizontalAlignment = HorizontalAlignment.Stretch;
            VerticalAlignment = VerticalAlignment.Top;

            image = new Image {
                Stretch = Stretch.None,
                HorizontalAlignment = HorizontalAlignment.Left,
                VerticalAlignment = VerticalAlignment.Top,
                SnapsToDevicePixels = true
            };
            RenderOptions.SetBitmapScalingMode(image, BitmapScalingMode.NearestNeighbor);
            RenderOptions.SetEdgeMode(image, EdgeMode.Aliased);

            overlay = new Canvas { IsHitTestVisible = false };

            hoverScale = new ScaleTransform(1, 1);
            hoverRect = new Rectangle {
                Width = MaxCellPx, Height = MaxCellPx,
                Stroke = Brushes.White, StrokeThickness = 1,
                Fill = Brushes.Transparent,
                RenderTransformOrigin = new Point(0.5, 0.5),
                RenderTransform = hoverScale,
                Visibility = Visibility.Collapsed
            };
            overlay.Children.Add(hoverRect);

            Children.Add(image);
            Children.Add(overlay);

            MouseMove  += OnMouseMoveCell;
            MouseLeave += OnMouseLeaveCell;
            MouseLeftButtonDown += OnMouseClickCell;
            SizeChanged += delegate { Rebuild(); };
        }

        public void SetEvents(IList<TraceRow> rows)
        {
            eventCount = rows.Count;
            nids = new uint[eventCount];
            for (int i = 0; i < eventCount; i++) nids[i] = rows[i].Nid;

            Dictionary<uint, int> counts = new Dictionary<uint, int>();
            for (int i = 0; i < eventCount; i++) {
                int c;
                counts.TryGetValue(nids[i], out c);
                counts[nids[i]] = c + 1;
            }

            // rank-based rarity. sort unique nids by count ascending;
            // each nid's rarity = 1 - (rank / nidCount), so the rarest
            // nid -> 1.0, the most common -> ~0.0. then square it so
            // only the truly rare tail lights up - the bulk of the
            // distribution stays flat gray and outliers pop.
            int nidCount = counts.Count;
            int[] sortedCounts = new int[nidCount];
            counts.Values.CopyTo(sortedCounts, 0);
            Array.Sort(sortedCounts);
            Dictionary<int, double> rarityByCount = new Dictionary<int, double>();
            for (int i = 0; i < nidCount; i++) {
                int c = sortedCounts[i];
                if (rarityByCount.ContainsKey(c)) continue;
                double rank = (i + 0.5) / nidCount;        // 0..1, low = rare
                double r = 1.0 - rank;
                rarityByCount[c] = r * r;                  // suppress mid-band
            }

            colors = new Color[eventCount];
            tooltips = new string[eventCount];
            Dictionary<int, Color> rampCache = new Dictionary<int, Color>();
            for (int i = 0; i < eventCount; i++) {
                uint nid = nids[i];
                int count = counts[nid];
                double rarity = rarityByCount[count];
                int bucket = (int)(rarity * 255);
                Color c;
                if (!rampCache.TryGetValue(bucket, out c)) {
                    c = HeatColor(rarity);
                    rampCache[bucket] = c;
                }
                colors[i] = c;
                TraceRow r = rows[i];
                string name = string.IsNullOrEmpty(r.Name) ? r.NidHex : r.Name;
                double pct = 100.0 * count / Math.Max(1, eventCount);
                tooltips[i] = string.Format(CultureInfo.InvariantCulture,
                    "#{0}  {1}\n{2} call{3}  ({4:0.0}%)",
                    r.Index, name, count, count == 1 ? "" : "s", pct);
            }

            Rebuild();
        }

        public void Clear()
        {
            eventCount = 0; nids = new uint[0]; colors = new Color[0]; tooltips = new string[0];
            image.Source = null;
            hoverRect.Visibility = Visibility.Collapsed;
            hoveredIndex = -1;
            ToolTip = null;
        }

        private void Rebuild()
        {
            if (eventCount == 0 || ActualWidth < 1) return;

            // keep cells legible. let the host ScrollViewer scroll
            // vertically when rows overflow the visible pane.
            cellPx = MaxCellPx;
            int w = (int)ActualWidth;
            cols = Math.Max(1, w / cellPx);
            rowCount = (eventCount + cols - 1) / cols;
            int pxW = cols * cellPx;
            int pxH = rowCount * cellPx;

            DrawingVisual dv = new DrawingVisual();
            using (DrawingContext dc = dv.RenderOpen()) {
                for (int i = 0; i < eventCount; i++) {
                    int cx = i % cols;
                    int cy = i / cols;
                    Brush b = new SolidColorBrush(colors[i]);
                    b.Freeze();
                    dc.DrawRectangle(b, null,
                        new Rect(cx * cellPx, cy * cellPx, cellPx, cellPx));
                }
            }
            RenderTargetBitmap bmp = new RenderTargetBitmap(
                Math.Max(1, pxW), Math.Max(1, pxH),
                96, 96, PixelFormats.Pbgra32);
            bmp.Render(dv);
            bmp.Freeze();
            image.Source = bmp;
            image.Width  = pxW;
            image.Height = pxH;
            // grow the control so the parent ScrollViewer can scroll.
            Height = pxH;

            hoverRect.Width = cellPx;
            hoverRect.Height = cellPx;
        }

        // single-ramp rarity heat: gray -> amber -> red. no per-nid hue,
        // so the eye is drawn to bright cells, not to color variety.
        //   t=0.0  dim gray   (very common)
        //   t=0.5  amber      (uncommon)
        //   t=1.0  bright red (rare / unique)
        private static Color HeatColor(double t)
        {
            if (t < 0) t = 0; else if (t > 1) t = 1;
            // gray (60,60,60) -> amber (230,160,40) -> red (255,40,40)
            double r, g, b;
            if (t < 0.5) {
                double k = t / 0.5;
                r =  60 + (230 -  60) * k;
                g =  60 + (160 -  60) * k;
                b =  60 + ( 40 -  60) * k;
            } else {
                double k = (t - 0.5) / 0.5;
                r = 230 + (255 - 230) * k;
                g = 160 + ( 40 - 160) * k;
                b =  40 + ( 40 -  40) * k;
            }
            return Color.FromRgb((byte)r, (byte)g, (byte)b);
        }

        private int CellAt(Point p)
        {
            if (cellPx <= 0) return -1;
            int cx = (int)(p.X / cellPx);
            int cy = (int)(p.Y / cellPx);
            if (cx < 0 || cx >= cols || cy < 0) return -1;
            int idx = cy * cols + cx;
            if (idx < 0 || idx >= eventCount) return -1;
            return idx;
        }

        private void OnMouseMoveCell(object sender, MouseEventArgs e)
        {
            Point p = e.GetPosition(this);
            int idx = CellAt(p);
            if (idx == hoveredIndex) return;
            hoveredIndex = idx;
            if (idx < 0) {
                hoverRect.Visibility = Visibility.Collapsed;
                ToolTip = null;
                return;
            }

            int cx = idx % cols;
            int cy = idx / cols;
            double cxPx = cx * cellPx + cellPx / 2.0;
            double cyPx = cy * cellPx + cellPx / 2.0;
            Canvas.SetLeft(hoverRect, cxPx - cellPx / 2.0);
            Canvas.SetTop (hoverRect, cyPx - cellPx / 2.0);
            hoverRect.Stroke = new SolidColorBrush(Colors.White);
            hoverRect.Fill   = new SolidColorBrush(colors[idx]);

            if (hoverRect.Visibility != Visibility.Visible)
                hoverRect.Visibility = Visibility.Visible;

            DoubleAnimation anim = new DoubleAnimation {
                To = HoverScale,
                Duration = new Duration(TimeSpan.FromMilliseconds(HoverAnimMs))
            };
            hoverScale.BeginAnimation(ScaleTransform.ScaleXProperty, anim);
            hoverScale.BeginAnimation(ScaleTransform.ScaleYProperty, anim);

            ToolTip = tooltips[idx];
        }

        private void OnMouseLeaveCell(object sender, MouseEventArgs e)
        {
            hoveredIndex = -1;
            hoverRect.Visibility = Visibility.Collapsed;
            ToolTip = null;
        }

        private void OnMouseClickCell(object sender, MouseButtonEventArgs e)
        {
            int idx = CellAt(e.GetPosition(this));
            if (idx < 0) return;
            Action<int> h = CellClicked;
            if (h != null) h(idx);
        }
    }
}