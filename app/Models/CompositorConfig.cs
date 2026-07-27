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
}

public sealed class CompositorRect
{
    [JsonPropertyName("x")] public int X { get; set; }
    [JsonPropertyName("y")] public int Y { get; set; }
    [JsonPropertyName("w")] public int W { get; set; }
    [JsonPropertyName("h")] public int H { get; set; }

    public static CompositorRect From(PixelRect r) => new() { X = r.X, Y = r.Y, W = r.W, H = r.H };
}
