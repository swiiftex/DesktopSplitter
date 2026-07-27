using System.Runtime.InteropServices;
using DesktopSplitter.Models;
using static DesktopSplitter.Interop.NativeMethods;

namespace DesktopSplitter.Interop;

public sealed class DisplayArrangeException : Exception
{
    public DisplayArrangeException(string message) : base(message) { }
}

/// <summary>
/// Places the virtual monitors in virtual-desktop space, immediately to the RIGHT of the
/// physical monitor, preserving the layout's relative geometry, and sets each to its
/// configured mode. Also owns primary-display switching, which requires re-basing every
/// monitor's position so the new primary sits at the virtual-desktop origin.
/// Uses ChangeDisplaySettingsEx with CDS_NORESET batching + a single final commit.
/// </summary>
public static class DisplayArranger
{
    public sealed record VirtualPlacement(
        string DeviceName, int Width, int Height, int X, int Y, int Col, int Row);

    /// <summary>
    /// A monitor's desired rect in virtual-desktop space. <paramref name="RefreshHz"/> of 0
    /// means "leave the current refresh rate alone".
    /// </summary>
    public sealed record MonitorPlacement(
        string DeviceName, int Width, int Height, int X, int Y, int RefreshHz, bool IsPrimary)
    {
        public int Right => X + Width;
        public int Bottom => Y + Height;
        public override string ToString()
            => $"{DeviceName} {Width}x{Height}+{X}+{Y}{(IsPrimary ? " [PRIMARY]" : string.Empty)}";
    }

    /// <summary>
    /// Computes desktop-space positions. Column widths / row heights come from the actual
    /// segment resolutions (not the physical monitor), so the virtual desktop stays gap-free
    /// even when segment resolutions differ from their on-screen rects.
    /// </summary>
    public static IReadOnlyList<VirtualPlacement> ComputePlacements(
        MonitorInfo physical,
        LayoutDefinition layout,
        IReadOnlyList<MonitorInfo> virtualMonitors,
        IReadOnlyList<(int Width, int Height)> resolutions)
    {
        if (virtualMonitors.Count != layout.SegmentCount)
            throw new DisplayArrangeException(
                $"Expected {layout.SegmentCount} virtual monitors, found {virtualMonitors.Count}.");
        if (resolutions.Count != layout.SegmentCount)
            throw new DisplayArrangeException(
                $"Expected {layout.SegmentCount} segment resolutions, got {resolutions.Count}.");

        LayoutGeometry.Tracks tracks = LayoutGeometry.ComputeTracks(layout, resolutions);
        int[] colWidth = tracks.ColumnWidths;
        int[] rowHeight = tracks.RowHeights;

        int baseX = physical.PositionX + physical.CurrentWidth;
        int baseY = physical.PositionY;

        var placements = new List<VirtualPlacement>(layout.SegmentCount);
        for (int i = 0; i < layout.SegmentCount; i++)
        {
            LayoutCell cell = layout.Cells[i];
            (int w, int h) = resolutions[i];

            int x = baseX;
            for (int c = 0; c < cell.Col; c++) x += colWidth[c];
            int y = baseY;
            for (int r = 0; r < cell.Row; r++) y += rowHeight[r];

            placements.Add(new VirtualPlacement(virtualMonitors[i].DeviceName, w, h, x, y, cell.Col, cell.Row));
        }
        return placements;
    }

    /// <summary>
    /// Applies mode + position for every virtual placement, then commits with a single
    /// ChangeDisplaySettingsEx(NULL, NULL, ...) so the desktop is only rebuilt once.
    /// Primary selection is NOT touched here — see <see cref="RebaseToPrimary"/> /
    /// <see cref="ApplyLayout"/>, which run afterwards.
    /// </summary>
    public static void Apply(
        MonitorInfo physical,
        IReadOnlyList<VirtualPlacement> placements,
        int refreshHz,
        Action<string>? log = null)
    {
        var problems = new List<string>();

        foreach (VirtualPlacement p in placements)
        {
            var placement = new MonitorPlacement(p.DeviceName, p.Width, p.Height, p.X, p.Y, refreshHz, false);

            int rc = SetPlacement(placement, CDS_UPDATEREGISTRY | CDS_NORESET, useRefresh: refreshHz > 1);
            if (rc != DISP_CHANGE_SUCCESSFUL)
            {
                log?.Invoke($"  {p.DeviceName}: {p.Width}x{p.Height}@{refreshHz}Hz rejected " +
                            $"({DispChangeToString(rc)}); retrying without an explicit refresh rate.");
                rc = SetPlacement(placement, CDS_UPDATEREGISTRY | CDS_NORESET, useRefresh: false);
            }

            if (rc != DISP_CHANGE_SUCCESSFUL)
                problems.Add($"{p.DeviceName} -> {p.Width}x{p.Height} at ({p.X},{p.Y}): {DispChangeToString(rc)}");
            else
                log?.Invoke($"  {p.DeviceName}: {p.Width}x{p.Height} at ({p.X},{p.Y})");
        }

        int commit = ChangeDisplaySettingsExApply(null, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero);
        if (commit != DISP_CHANGE_SUCCESSFUL)
            problems.Add($"final apply: {DispChangeToString(commit)}");

        if (problems.Count > 0)
            throw new DisplayArrangeException(
                "Could not arrange the virtual monitors: " + string.Join("; ", problems) + ".");
    }

    // ------------------------------------------------------------------ primary switching

    /// <summary>
    /// Shifts every monitor so that <paramref name="primaryDevice"/> lands on the virtual-desktop
    /// origin (0,0) and is flagged primary. Windows requires the primary display to be at (0,0),
    /// so promoting a different monitor necessarily re-bases all the others by the same vector —
    /// which leaves every relative offset, and therefore the whole arrangement, untouched.
    /// Pure function: no display APIs are called.
    /// </summary>
    public static IReadOnlyList<MonitorPlacement> RebaseToPrimary(
        IReadOnlyList<MonitorPlacement> placements, string primaryDevice)
    {
        if (placements.Count == 0)
            throw new DisplayArrangeException("Cannot re-base an empty monitor set.");

        MonitorPlacement? origin = placements.FirstOrDefault(
            p => string.Equals(p.DeviceName, primaryDevice, StringComparison.OrdinalIgnoreCase));
        if (origin is null)
        {
            throw new DisplayArrangeException(
                $"Monitor '{primaryDevice}' is not in the current display set, so it cannot be made primary. " +
                $"Known: {string.Join(", ", placements.Select(p => p.DeviceName))}.");
        }

        int dx = -origin.X;
        int dy = -origin.Y;

        return placements
            .Select(p => p with
            {
                X = p.X + dx,
                Y = p.Y + dy,
                IsPrimary = string.Equals(p.DeviceName, primaryDevice, StringComparison.OrdinalIgnoreCase),
            })
            .ToList();
    }

    /// <summary>
    /// Writes a whole desktop layout in one batch. The primary is written first (Windows is
    /// happiest re-anchoring the origin before the satellites move), then every other monitor,
    /// then a single commit.
    /// </summary>
    public static void ApplyLayout(IReadOnlyList<MonitorPlacement> placements, Action<string>? log = null)
    {
        if (placements.Count == 0) return;

        int primaryCount = placements.Count(p => p.IsPrimary);
        if (primaryCount != 1)
            throw new DisplayArrangeException($"Exactly one monitor must be primary, got {primaryCount}.");

        MonitorPlacement primary = placements.First(p => p.IsPrimary);
        if (primary.X != 0 || primary.Y != 0)
            throw new DisplayArrangeException(
                $"The primary monitor must sit at (0,0); {primary.DeviceName} is at ({primary.X},{primary.Y}).");

        var problems = new List<string>();

        foreach (MonitorPlacement p in placements.OrderByDescending(p => p.IsPrimary))
        {
            uint flags = CDS_UPDATEREGISTRY | CDS_NORESET;
            if (p.IsPrimary) flags |= CDS_SET_PRIMARY;

            int rc = SetPlacement(p, flags, useRefresh: p.RefreshHz > 1);
            if (rc != DISP_CHANGE_SUCCESSFUL && p.RefreshHz > 1)
            {
                log?.Invoke($"  {p.DeviceName}: rejected at {p.RefreshHz}Hz ({DispChangeToString(rc)}); " +
                            "retrying without an explicit refresh rate.");
                rc = SetPlacement(p, flags, useRefresh: false);
            }

            if (rc != DISP_CHANGE_SUCCESSFUL)
                problems.Add($"{p.DeviceName} -> ({p.X},{p.Y}): {DispChangeToString(rc)}");
            else
                log?.Invoke($"  {p.DeviceName}: {p.Width}x{p.Height} at ({p.X},{p.Y})" +
                            (p.IsPrimary ? "   <-- PRIMARY" : string.Empty));
        }

        int commit = ChangeDisplaySettingsExApply(null, IntPtr.Zero, IntPtr.Zero, 0, IntPtr.Zero);
        if (commit != DISP_CHANGE_SUCCESSFUL)
            problems.Add($"final apply: {DispChangeToString(commit)}");

        if (problems.Count > 0)
            throw new DisplayArrangeException(
                "Could not set the primary display: " + string.Join("; ", problems) + ".");
    }

    /// <summary>Snapshot of the live desktop as placements (refresh left untouched).</summary>
    public static IReadOnlyList<MonitorPlacement> SnapshotLayout(IEnumerable<MonitorInfo> monitors)
        => monitors
            .Select(m => new MonitorPlacement(
                m.DeviceName, m.CurrentWidth, m.CurrentHeight, m.PositionX, m.PositionY, 0, m.IsPrimary))
            .ToList();

    private static int SetPlacement(MonitorPlacement p, uint flags, bool useRefresh)
    {
        var dm = DEVMODE.Create();
        if (!EnumDisplaySettingsEx(p.DeviceName, ENUM_CURRENT_SETTINGS, ref dm, 0))
            dm = DEVMODE.Create();

        dm.dmDeviceName = string.Empty;
        dm.dmFormName = string.Empty;
        dm.dmSize = (ushort)Marshal.SizeOf<DEVMODE>();
        dm.dmPelsWidth = (uint)p.Width;
        dm.dmPelsHeight = (uint)p.Height;
        dm.dmBitsPerPel = 32;
        dm.dmPositionX = p.X;
        dm.dmPositionY = p.Y;
        dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;

        if (useRefresh && p.RefreshHz > 1)
        {
            dm.dmDisplayFrequency = (uint)p.RefreshHz;
            dm.dmFields |= DM_DISPLAYFREQUENCY;
        }

        return ChangeDisplaySettingsEx(p.DeviceName, ref dm, IntPtr.Zero, flags, IntPtr.Zero);
    }
}
