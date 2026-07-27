namespace DesktopSplitter.Models;

/// <summary>
/// Picks where the keep-or-revert countdown appears. The whole point of the safety dialog is
/// that it stays readable when the target display has just gone dark or started flickering, so
/// it must land on a DIFFERENT monitor whenever the desk has one.
/// Pure functions — unit-testable without a display.
/// </summary>
public static class CountdownPlacement
{
    /// <summary>
    /// Chooses the monitor to show the countdown on: any monitor other than
    /// <paramref name="targetDevice"/>, preferring the primary, then the lowest device index.
    /// Falls back to the primary (or the first monitor) on a single-display desk.
    /// </summary>
    public static MonitorInfo? ChooseMonitor(IReadOnlyList<MonitorInfo> monitors, string targetDevice)
    {
        if (monitors.Count == 0) return null;

        var others = monitors
            .Where(m => !string.Equals(m.DeviceName, targetDevice, StringComparison.OrdinalIgnoreCase))
            .ToList();

        if (others.Count > 0)
        {
            return others.FirstOrDefault(m => m.IsPrimary)
                   ?? others.OrderBy(m => m.Index).First();
        }

        // Single display, or the target IS the only one: nothing better than the primary.
        return monitors.FirstOrDefault(m => m.IsPrimary) ?? monitors[0];
    }

    /// <summary>
    /// Centres a window of the given physical pixel size on a monitor, clamped so it never
    /// starts outside the monitor even if the window is larger than the display.
    /// </summary>
    public static PixelRect Center(MonitorInfo monitor, int windowWidth, int windowHeight)
    {
        int w = Math.Max(1, windowWidth);
        int h = Math.Max(1, windowHeight);

        int x = monitor.PositionX + (monitor.CurrentWidth - w) / 2;
        int y = monitor.PositionY + (monitor.CurrentHeight - h) / 2;

        if (w >= monitor.CurrentWidth) x = monitor.PositionX;
        if (h >= monitor.CurrentHeight) y = monitor.PositionY;

        return new PixelRect(x, y, w, h);
    }
}
