namespace DesktopSplitter.Models;

/// <summary>A segment's rectangle on the editor canvas, in device-independent pixels.</summary>
public readonly record struct ZoneRect(double X, double Y, double W, double H);

/// <summary>
/// Grid maths shared by the interactive editor and the apply path.
///
/// A layout is a grid of column/row tracks. Track sizes come from the edited segment sizes
/// (in monitor pixels); every derived figure — canvas zone rects, splitter positions and the
/// physRects handed to the compositor — is produced by normalising those tracks onto the
/// monitor's real dimensions. That way the on-screen split mirrors the editor exactly even
/// when the typed sizes do not sum to the monitor dimension.
/// </summary>
public static class LayoutGeometry
{
    public const int MinSegmentDimension = 640;
    public const int MaxSegmentDimension = 7680;

    /// <summary>Column widths / row heights in monitor pixels, derived from the segment sizes.</summary>
    public sealed record Tracks(int[] ColumnWidths, int[] RowHeights);

    /// <summary>
    /// Track sizes from the current segment sizes. Non-spanning cells define a track's size
    /// (largest wins when several share it); spanning cells only grow the last track they
    /// cover, and only if they would not otherwise fit.
    /// </summary>
    public static Tracks ComputeTracks(LayoutDefinition layout, IReadOnlyList<(int Width, int Height)> sizes)
    {
        var cols = new int[layout.Columns];
        var rows = new int[layout.Rows];

        for (int i = 0; i < layout.SegmentCount && i < sizes.Count; i++)
        {
            LayoutCell cell = layout.Cells[i];
            (int w, int h) = sizes[i];
            if (cell.ColSpan == 1) cols[cell.Col] = Math.Max(cols[cell.Col], w);
            if (cell.RowSpan == 1) rows[cell.Row] = Math.Max(rows[cell.Row], h);
        }

        for (int i = 0; i < layout.SegmentCount && i < sizes.Count; i++)
        {
            LayoutCell cell = layout.Cells[i];
            (int w, int h) = sizes[i];

            if (cell.ColSpan > 1)
            {
                int span = 0;
                for (int c = cell.Col; c <= cell.LastCol; c++) span += cols[c];
                if (span < w) cols[cell.LastCol] += w - span;
            }
            if (cell.RowSpan > 1)
            {
                int span = 0;
                for (int r = cell.Row; r <= cell.LastRow; r++) span += rows[r];
                if (span < h) rows[cell.LastRow] += h - span;
            }
        }

        for (int c = 0; c < cols.Length; c++) if (cols[c] <= 0) cols[c] = MinSegmentDimension;
        for (int r = 0; r < rows.Length; r++) if (rows[r] <= 0) rows[r] = MinSegmentDimension;

        return new Tracks(cols, rows);
    }

    /// <summary>
    /// Scales <paramref name="tracks"/> so they sum to exactly <paramref name="total"/>.
    /// Every track but the last is rounded independently; the last absorbs the remainder,
    /// so the result always tiles perfectly.
    /// </summary>
    public static int[] Normalize(IReadOnlyList<int> tracks, int total)
    {
        int n = tracks.Count;
        var result = new int[n];
        if (n == 0) return result;
        if (n == 1) { result[0] = Math.Max(1, total); return result; }

        long sum = 0;
        for (int i = 0; i < n; i++) sum += Math.Max(1, tracks[i]);
        if (sum <= 0) sum = n;

        int accumulated = 0;
        for (int i = 0; i < n - 1; i++)
        {
            result[i] = Math.Max(1, (int)Math.Round(Math.Max(1, tracks[i]) * (double)total / sum));
            accumulated += result[i];
        }
        result[n - 1] = total - accumulated;

        // Pathological inputs (e.g. a track far larger than the monitor) can drive the
        // remainder non-positive; claw pixels back from the largest tracks.
        while (result[n - 1] < 1)
        {
            int largest = 0;
            for (int i = 0; i < n - 1; i++) if (result[i] > result[largest]) largest = i;
            if (result[largest] <= 1) { result[n - 1] = 1; break; }
            result[largest]--;
            result[n - 1]++;
        }
        return result;
    }

    /// <summary>Running offsets for a track list: <c>offsets[i]</c> is the start of track i.</summary>
    public static int[] Offsets(IReadOnlyList<int> tracks)
    {
        var offsets = new int[tracks.Count + 1];
        for (int i = 0; i < tracks.Count; i++) offsets[i + 1] = offsets[i] + tracks[i];
        return offsets;
    }

    /// <summary>
    /// Segment rectangles in physical-monitor-local pixels, derived from the edited sizes.
    /// Tracks are normalised per axis (last track takes the remainder) so the rects tile the
    /// monitor exactly regardless of what the user typed.
    /// </summary>
    public static IReadOnlyList<PixelRect> ComputePhysRects(
        LayoutDefinition layout, int monitorWidth, int monitorHeight,
        IReadOnlyList<(int Width, int Height)> sizes)
    {
        Tracks tracks = ComputeTracks(layout, sizes);
        int[] cols = Normalize(tracks.ColumnWidths, monitorWidth);
        int[] rows = Normalize(tracks.RowHeights, monitorHeight);
        int[] colOffsets = Offsets(cols);
        int[] rowOffsets = Offsets(rows);

        var rects = new List<PixelRect>(layout.SegmentCount);
        foreach (LayoutCell cell in layout.Cells)
        {
            int x = colOffsets[cell.Col];
            int y = rowOffsets[cell.Row];
            int w = colOffsets[cell.LastCol + 1] - x;
            int h = rowOffsets[cell.LastRow + 1] - y;
            rects.Add(new PixelRect(x, y, Math.Max(1, w), Math.Max(1, h)));
        }
        return rects;
    }

    /// <summary>
    /// Zone rectangles on a canvas of the given size, proportional to the current track sizes.
    /// Used purely for drawing, so it works even when the sizes do not sum to the monitor.
    /// </summary>
    public static IReadOnlyList<ZoneRect> ComputeZoneRects(
        LayoutDefinition layout, double canvasWidth, double canvasHeight,
        IReadOnlyList<(int Width, int Height)> sizes)
    {
        Tracks tracks = ComputeTracks(layout, sizes);
        double[] colEdges = FractionEdges(tracks.ColumnWidths, canvasWidth);
        double[] rowEdges = FractionEdges(tracks.RowHeights, canvasHeight);

        var zones = new List<ZoneRect>(layout.SegmentCount);
        foreach (LayoutCell cell in layout.Cells)
        {
            double x = colEdges[cell.Col];
            double y = rowEdges[cell.Row];
            zones.Add(new ZoneRect(x, y, colEdges[cell.LastCol + 1] - x, rowEdges[cell.LastRow + 1] - y));
        }
        return zones;
    }

    private static double[] FractionEdges(IReadOnlyList<int> tracks, double extent)
    {
        var edges = new double[tracks.Count + 1];
        double sum = 0;
        for (int i = 0; i < tracks.Count; i++) sum += Math.Max(1, tracks[i]);
        if (sum <= 0) sum = 1;

        double accumulated = 0;
        for (int i = 0; i < tracks.Count; i++)
        {
            accumulated += Math.Max(1, tracks[i]);
            edges[i + 1] = accumulated / sum * extent;
        }
        edges[tracks.Count] = extent;   // kill accumulated FP drift
        return edges;
    }

    /// <summary>
    /// Clamps a proposed leading-track size so both neighbours stay within
    /// [<see cref="MinSegmentDimension"/>, <see cref="MaxSegmentDimension"/>] while their
    /// sum stays exactly <paramref name="pairTotal"/> (auto-fill).
    /// </summary>
    public static int ClampSplit(int proposedLeading, int pairTotal)
    {
        int lower = Math.Max(MinSegmentDimension, pairTotal - MaxSegmentDimension);
        int upper = Math.Min(MaxSegmentDimension, pairTotal - MinSegmentDimension);

        // A pair total below 2 * min cannot satisfy both; split it as evenly as possible.
        if (upper < lower) return pairTotal / 2;

        return Math.Clamp(proposedLeading, lower, upper);
    }
}
