namespace DesktopSplitter.Models;

public enum LayoutKind
{
    TwoVertical = 0,          // side by side
    TwoHorizontal = 1,        // stacked
    ThreeColumns = 2,
    BigLeftTwoStackedRight = 3,
    Grid2x2 = 4,
    FourColumns = 5,

    /// <summary>
    /// Three columns where the centre is a true 16:9 rectangle for the monitor's height and
    /// the sides take the remainder. Segment 1 is the CENTRE (see <see cref="LayoutDefinition"/>).
    /// </summary>
    SixteenNineCenter = 6,
}

/// <summary>One segment of a layout.</summary>
/// <param name="Fx">Left edge as a fraction of the physical monitor width.</param>
/// <param name="Fy">Top edge as a fraction of the physical monitor height.</param>
/// <param name="Fw">Width fraction.</param>
/// <param name="Fh">Height fraction.</param>
/// <param name="Col">Grid column (used to lay virtual monitors out in desktop space).</param>
/// <param name="Row">Grid row.</param>
/// <param name="ColSpan">Columns spanned.</param>
/// <param name="RowSpan">Rows spanned.</param>
public sealed record LayoutCell(
    double Fx, double Fy, double Fw, double Fh,
    int Col, int Row, int ColSpan, int RowSpan)
{
    /// <summary>Index of the rightmost column this cell occupies.</summary>
    public int LastCol => Col + ColSpan - 1;

    /// <summary>Index of the bottom row this cell occupies.</summary>
    public int LastRow => Row + RowSpan - 1;
}

/// <summary>A rectangle in the layout preview thumbnail, in device-independent pixels.</summary>
public sealed record PreviewBox(double X, double Y, double W, double H, int Index);

public sealed class LayoutDefinition
{
    public const double PreviewWidth = 132;
    public const double PreviewHeight = 76;
    private const double PreviewGap = 3;

    public LayoutKind Kind { get; }
    public string Name { get; }
    public string Description { get; }
    public int Columns { get; }
    public int Rows { get; }
    public IReadOnlyList<LayoutCell> Cells { get; }
    public IReadOnlyList<PreviewBox> PreviewBoxes { get; }

    /// <summary>
    /// Draggable vertical grid lines, identified by the column they sit AFTER
    /// (line c separates column c from column c+1).
    /// </summary>
    public IReadOnlyList<int> VerticalSplitLines { get; }

    /// <summary>Draggable horizontal grid lines, identified by the row they sit AFTER.</summary>
    public IReadOnlyList<int> HorizontalSplitLines { get; }

    public int SegmentCount => Cells.Count;

    /// <summary>
    /// Optional per-layout override for the "reset to defaults" sizes, in SEGMENT order.
    /// Layouts without one fall back to the equal subdivision implied by the cell fractions.
    /// </summary>
    private readonly Func<int, int, IReadOnlyList<(int Width, int Height)>>? _defaultSizes;

    private LayoutDefinition(LayoutKind kind, string name, string description,
                             int columns, int rows,
                             IReadOnlyList<int> verticalSplitLines,
                             IReadOnlyList<int> horizontalSplitLines,
                             IReadOnlyList<LayoutCell> cells,
                             Func<int, int, IReadOnlyList<(int Width, int Height)>>? defaultSizes = null)
    {
        Kind = kind;
        Name = name;
        Description = description;
        Columns = columns;
        Rows = rows;
        VerticalSplitLines = verticalSplitLines;
        HorizontalSplitLines = horizontalSplitLines;
        Cells = cells;
        PreviewBoxes = BuildPreview(cells);
        _defaultSizes = defaultSizes;
    }

    /// <summary>
    /// The sizes the Reset button restores, in SEGMENT order (segment i -&gt; result[i]),
    /// which is not necessarily left-to-right track order.
    /// </summary>
    public IReadOnlyList<(int Width, int Height)> ComputeDefaultSegmentSizes(int physWidth, int physHeight)
        => _defaultSizes?.Invoke(physWidth, physHeight)
           ?? ComputePhysRects(physWidth, physHeight).Select(r => (r.W, r.H)).ToList();

    private static IReadOnlyList<PreviewBox> BuildPreview(IReadOnlyList<LayoutCell> cells)
    {
        var boxes = new List<PreviewBox>(cells.Count);
        for (int i = 0; i < cells.Count; i++)
        {
            LayoutCell c = cells[i];
            double x = c.Fx * PreviewWidth;
            double y = c.Fy * PreviewHeight;
            double w = c.Fw * PreviewWidth;
            double h = c.Fh * PreviewHeight;
            boxes.Add(new PreviewBox(
                x + PreviewGap / 2,
                y + PreviewGap / 2,
                Math.Max(1, w - PreviewGap),
                Math.Max(1, h - PreviewGap),
                i));
        }
        return boxes;
    }

    /// <summary>
    /// Default segment rectangles in physical-monitor-local pixels (equal subdivision).
    /// Boundaries are rounded consistently so the segments tile the monitor exactly.
    /// These are the "reset to defaults" values; the rects that actually ship to the
    /// compositor come from <see cref="LayoutGeometry.ComputePhysRects"/>, which honours
    /// whatever the user dragged or typed.
    /// </summary>
    public IReadOnlyList<PixelRect> ComputePhysRects(int physWidth, int physHeight)
    {
        var rects = new List<PixelRect>(Cells.Count);
        foreach (LayoutCell c in Cells)
        {
            int x0 = (int)Math.Round(c.Fx * physWidth);
            int y0 = (int)Math.Round(c.Fy * physHeight);
            int x1 = (int)Math.Round((c.Fx + c.Fw) * physWidth);
            int y1 = (int)Math.Round((c.Fy + c.Fh) * physHeight);
            rects.Add(new PixelRect(x0, y0, Math.Max(1, x1 - x0), Math.Max(1, y1 - y0)));
        }
        return rects;
    }

    /// <summary>Segments whose right edge touches vertical line <paramref name="line"/>.</summary>
    public IEnumerable<int> SegmentsLeftOf(int line)
        => Enumerable.Range(0, Cells.Count).Where(i => Cells[i].LastCol == line);

    /// <summary>Segments whose left edge touches vertical line <paramref name="line"/>.</summary>
    public IEnumerable<int> SegmentsRightOf(int line)
        => Enumerable.Range(0, Cells.Count).Where(i => Cells[i].Col == line + 1);

    /// <summary>Segments whose bottom edge touches horizontal line <paramref name="line"/>.</summary>
    public IEnumerable<int> SegmentsAbove(int line)
        => Enumerable.Range(0, Cells.Count).Where(i => Cells[i].LastRow == line);

    /// <summary>Segments whose top edge touches horizontal line <paramref name="line"/>.</summary>
    public IEnumerable<int> SegmentsBelow(int line)
        => Enumerable.Range(0, Cells.Count).Where(i => Cells[i].Row == line + 1);

    private static LayoutDefinition Make(LayoutKind kind, string name, string desc,
                                         int cols, int rows,
                                         int[] verticalSplitLines, int[] horizontalSplitLines,
                                         params LayoutCell[] cells)
        => new(kind, name, desc, cols, rows, verticalSplitLines, horizontalSplitLines, cells);

    private static LayoutDefinition MakeCustom(LayoutKind kind, string name, string desc,
                                               int cols, int rows,
                                               int[] verticalSplitLines, int[] horizontalSplitLines,
                                               Func<int, int, IReadOnlyList<(int Width, int Height)>> defaultSizes,
                                               params LayoutCell[] cells)
        => new(kind, name, desc, cols, rows, verticalSplitLines, horizontalSplitLines, cells, defaultSizes);

    /// <summary>
    /// Defaults for <see cref="LayoutKind.SixteenNineCenter"/>, returned in SEGMENT order:
    /// [0] = centre, [1] = left, [2] = right.
    ///
    /// The centre is a true 16:9 rectangle for the monitor's height. If that leaves the sides
    /// under <see cref="LayoutGeometry.MinSegmentDimension"/> the centre is clamped down so both
    /// sides can reach the minimum; on a genuinely 16:9 monitor the centre ends up squeezed and
    /// the normal per-segment validation tells the user. The leftover width is halved, with the
    /// LEFT segment absorbing the odd pixel.
    /// </summary>
    public static IReadOnlyList<(int Width, int Height)> SixteenNineCenterDefaults(int monitorWidth, int monitorHeight)
    {
        int min = LayoutGeometry.MinSegmentDimension;

        int ideal = (int)Math.Round(monitorHeight * 16.0 / 9.0);
        int widest = monitorWidth - 2 * min;          // centre width that still leaves both sides at min
        int center = Math.Clamp(Math.Min(ideal, widest), min, LayoutGeometry.MaxSegmentDimension);

        int sides = Math.Max(2, monitorWidth - center);
        int right = sides / 2;
        int left = sides - right;                      // left takes the rounding

        return new[]
        {
            (Math.Max(1, center), Math.Max(1, monitorHeight)),   // segment 1 — CENTRE
            (Math.Max(1, left),   Math.Max(1, monitorHeight)),   // segment 2 — LEFT
            (Math.Max(1, right),  Math.Max(1, monitorHeight)),   // segment 3 — RIGHT
        };
    }

    public static IReadOnlyList<LayoutDefinition> All { get; } = new[]
    {
        Make(LayoutKind.TwoVertical, "2 — side by side", "Two halves split by a vertical line", 2, 1,
            new[] { 0 }, Array.Empty<int>(),
            new LayoutCell(0.0, 0.0, 0.5, 1.0, 0, 0, 1, 1),
            new LayoutCell(0.5, 0.0, 0.5, 1.0, 1, 0, 1, 1)),

        Make(LayoutKind.TwoHorizontal, "2 — stacked", "Two halves split by a horizontal line", 1, 2,
            Array.Empty<int>(), new[] { 0 },
            new LayoutCell(0.0, 0.0, 1.0, 0.5, 0, 0, 1, 1),
            new LayoutCell(0.0, 0.5, 1.0, 0.5, 0, 1, 1, 1)),

        Make(LayoutKind.ThreeColumns, "3 — columns", "Three equal columns", 3, 1,
            new[] { 0, 1 }, Array.Empty<int>(),
            new LayoutCell(0.0,       0.0, 1.0 / 3.0, 1.0, 0, 0, 1, 1),
            new LayoutCell(1.0 / 3.0, 0.0, 1.0 / 3.0, 1.0, 1, 0, 1, 1),
            new LayoutCell(2.0 / 3.0, 0.0, 1.0 / 3.0, 1.0, 2, 0, 1, 1)),

        Make(LayoutKind.BigLeftTwoStackedRight, "3 — big left + 2 right",
             "One tall segment on the left, two stacked on the right", 2, 2,
            new[] { 0 }, new[] { 0 },
            new LayoutCell(0.0, 0.0, 0.5, 1.0, 0, 0, 1, 2),
            new LayoutCell(0.5, 0.0, 0.5, 0.5, 1, 0, 1, 1),
            new LayoutCell(0.5, 0.5, 0.5, 0.5, 1, 1, 1, 1)),

        // Segment 1 is the CENTRE (column track 1), so it becomes driver monitor index 0,
        // gets promoted to primary, and is where games and the shell land. Segments 2 and 3
        // are the left (track 0) and right (track 2) fillers — segment order deliberately
        // differs from track order; every consumer keys off LayoutCell.Col, never the index.
        MakeCustom(LayoutKind.SixteenNineCenter, "3 — 16:9 centre",
             "True 16:9 centre for gaming, side rails fill the rest", 3, 1,
            new[] { 0, 1 }, Array.Empty<int>(),
            SixteenNineCenterDefaults,
            new LayoutCell(0.25, 0.0, 0.50, 1.0, 1, 0, 1, 1),   // segment 1 — centre
            new LayoutCell(0.00, 0.0, 0.25, 1.0, 0, 0, 1, 1),   // segment 2 — left
            new LayoutCell(0.75, 0.0, 0.25, 1.0, 2, 0, 1, 1)),  // segment 3 — right

        Make(LayoutKind.Grid2x2, "4 — 2x2 grid", "Four quadrants", 2, 2,
            new[] { 0 }, new[] { 0 },
            new LayoutCell(0.0, 0.0, 0.5, 0.5, 0, 0, 1, 1),
            new LayoutCell(0.5, 0.0, 0.5, 0.5, 1, 0, 1, 1),
            new LayoutCell(0.0, 0.5, 0.5, 0.5, 0, 1, 1, 1),
            new LayoutCell(0.5, 0.5, 0.5, 0.5, 1, 1, 1, 1)),

        Make(LayoutKind.FourColumns, "4 — columns", "Four equal columns", 4, 1,
            new[] { 0, 1, 2 }, Array.Empty<int>(),
            new LayoutCell(0.00, 0.0, 0.25, 1.0, 0, 0, 1, 1),
            new LayoutCell(0.25, 0.0, 0.25, 1.0, 1, 0, 1, 1),
            new LayoutCell(0.50, 0.0, 0.25, 1.0, 2, 0, 1, 1),
            new LayoutCell(0.75, 0.0, 0.25, 1.0, 3, 0, 1, 1)),
    };

    public static LayoutDefinition FromKind(LayoutKind kind)
        => All.FirstOrDefault(l => l.Kind == kind) ?? All[0];

    public override string ToString() => Name;
}
