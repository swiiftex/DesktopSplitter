using DesktopSplitter.Interop;

namespace DesktopSplitter.Models;

/// <summary>
/// A monitor as reported by QueryDisplayConfig + EnumDisplaySettingsEx.
/// </summary>
public sealed class MonitorInfo
{
    /// <summary>GDI device name, e.g. <c>\\.\DISPLAY1</c>.</summary>
    public string DeviceName { get; init; } = string.Empty;

    /// <summary>EDID friendly name, e.g. "DELL U2723QE".</summary>
    public string FriendlyName { get; init; } = string.Empty;

    /// <summary>Display adapter device path (DISPLAYCONFIG_ADAPTER_NAME).</summary>
    public string AdapterPath { get; init; } = string.Empty;

    /// <summary>Monitor device interface path (DISPLAYCONFIG_TARGET_DEVICE_NAME).</summary>
    public string MonitorDevicePath { get; init; } = string.Empty;

    /// <summary>Three-letter EDID manufacturer code, e.g. "DEL", "DSP".</summary>
    public string EdidManufacturer { get; init; } = string.Empty;

    public int CurrentWidth { get; init; }
    public int CurrentHeight { get; init; }

    /// <summary>Top-left position in virtual desktop space.</summary>
    public int PositionX { get; init; }
    public int PositionY { get; init; }

    /// <summary>Highest resolution offered by the mode list (treated as native).</summary>
    public int MaxWidth { get; init; }
    public int MaxHeight { get; init; }

    /// <summary>Highest refresh available at the native resolution, whole Hz.</summary>
    public int MaxRefreshHz { get; init; }

    /// <summary>
    /// Same refresh expressed in millihertz. Exact (e.g. 164999) when the monitor
    /// is currently running at its max rate — that is the only point at which
    /// DisplayConfig exposes the true rational; otherwise Hz * 1000.
    /// </summary>
    public uint MaxRefreshMillihertz { get; init; }

    /// <summary>Refresh rate of the mode the monitor is running RIGHT NOW, whole Hz.</summary>
    public int CurrentRefreshHz { get; init; }

    /// <summary>Current refresh in millihertz — exact rational from DisplayConfig when available.</summary>
    public uint CurrentRefreshMillihertz { get; init; }

    /// <summary>Numeric suffix of the GDI device name (the n in <c>\\.\DISPLAYn</c>).</summary>
    public int Index { get; init; }

    public bool IsPrimary { get; init; }

    /// <summary>True when this target belongs to the DesktopSplitter virtual driver.</summary>
    public bool IsDeskSplitVirtual { get; init; }

    /// <summary>Parsed EDID base block, when Windows has one cached for this monitor.</summary>
    public EdidInfo? Edid { get; init; }

    /// <summary>
    /// Driver monitor index decoded from our stamped EDID serial
    /// (0xD5000001 + index), or -1 when this is not one of our monitors / no EDID.
    /// </summary>
    public int DeskSplitMonitorIndex { get; init; } = -1;

    /// <summary>
    /// Human label for the monitor. Never blank: monitors with no EDID-provided friendly
    /// name (common for some DisplayPort/MST and adapter-attached targets) fall back to
    /// "Display n".
    /// </summary>
    public string DisplayLabel =>
        string.IsNullOrWhiteSpace(FriendlyName)
            ? $"Display {(Index == int.MaxValue || Index <= 0 ? 1 : Index)}"
            : FriendlyName.Trim();

    /// <summary>
    /// Combo-box label: "&lt;name&gt; — \\.\DISPLAYn — &lt;WxH&gt; @ &lt;maxHz&gt;Hz".
    /// Guaranteed non-empty even if every field is missing.
    /// </summary>
    public string Caption
    {
        get
        {
            string device = string.IsNullOrWhiteSpace(DeviceName) ? "(unknown device)" : DeviceName;
            string mode = CurrentWidth > 0 && CurrentHeight > 0
                ? $"{CurrentWidth}x{CurrentHeight}"
                : "unknown size";
            string hz = MaxRefreshHz > 0 ? $" @ {MaxRefreshHz}Hz" : string.Empty;
            string primary = IsPrimary ? "  (primary)" : string.Empty;
            return $"{DisplayLabel} — {device} — {mode}{hz}{primary}";
        }
    }

    /// <summary>Current mode readout, e.g. "5120x1440 @ 240 Hz".</summary>
    public string CurrentModeText =>
        CurrentWidth > 0 && CurrentHeight > 0
            ? $"{CurrentWidth}x{CurrentHeight} @ {(CurrentRefreshHz > 0 ? CurrentRefreshHz : MaxRefreshHz)} Hz"
            : "unknown";

    public override string ToString() => Caption;
}
