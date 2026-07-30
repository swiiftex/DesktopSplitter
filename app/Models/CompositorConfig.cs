using System.Text.Json.Serialization;

namespace DesktopSplitter.Models;

/// <summary>
/// %ProgramData%\DesktopSplitter\config.json — control app -> compositor.
/// Schema is fixed by docs/ARCHITECTURE.md; do not rename members.
/// </summary>
public sealed class CompositorConfig
{
    [JsonPropertyName("physicalDevice")]
    public string PhysicalDevice { get; set; } = string.Empty;

    [JsonPropertyName("refreshMillihertz")]
    public uint RefreshMillihertz { get; set; }

    /// <summary>
    /// 0-based index of the segment promoted to Windows' primary display. Informational for the
    /// compositor; the app performs the promotion. Absent/negative means segment 0.
    /// </summary>
    [JsonPropertyName("primarySegment")]
    public int PrimarySegment { get; set; }

    /// <summary>
    /// 0-based index of the segment new application windows should open on, or -1 for off.
    /// Lets a side segment hold the full taskbar while apps still launch on the centre one.
    /// </summary>
    [JsonPropertyName("launchSegment")]
    public int LaunchSegment { get; set; } = -1;

    [JsonPropertyName("segments")]
    public List<CompositorSegment> Segments { get; set; } = new();
}

public sealed class CompositorSegment
{
    [JsonPropertyName("virtualDevice")]
    public string VirtualDevice { get; set; } = string.Empty;

    [JsonPropertyName("width")]
    public int Width { get; set; }

    [JsonPropertyName("height")]
    public int Height { get; set; }

    [JsonPropertyName("physRect")]
    public CompositorRect PhysRect { get; set; } = new();

    /// <summary>
    /// Whether Windows' taskbar should appear on this segment. Backward compatible: an absent
    /// value means true, matching the behaviour before this field existed.
    /// </summary>
    [JsonPropertyName("showTaskbar")]
    public bool ShowTaskbar { get; set; } = true;
}

public sealed class CompositorRect
{
    [JsonPropertyName("x")] public int X { get; set; }
    [JsonPropertyName("y")] public int Y { get; set; }
    [JsonPropertyName("w")] public int W { get; set; }
    [JsonPropertyName("h")] public int H { get; set; }

    public static CompositorRect From(PixelRect r) => new() { X = r.X, Y = r.Y, W = r.W, H = r.H };
}
