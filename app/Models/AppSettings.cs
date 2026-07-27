using System.Text.Json.Serialization;

namespace DesktopSplitter.Models;

/// <summary>%APPDATA%\DesktopSplitter\settings.json — last applied UI state.</summary>
public sealed class AppSettings
{
    [JsonPropertyName("physicalDevice")]
    public string PhysicalDevice { get; set; } = string.Empty;

    [JsonPropertyName("physicalFriendlyName")]
    public string PhysicalFriendlyName { get; set; } = string.Empty;

    [JsonPropertyName("layout")]
    public LayoutKind Layout { get; set; } = LayoutKind.TwoVertical;

    [JsonPropertyName("segments")]
    public List<SegmentSize> Segments { get; set; } = new();

    /// <summary>
    /// The GDI device name (<c>\\.\DISPLAYn</c>) that was Windows' primary display immediately
    /// BEFORE the last Apply. Revert restores primary to this monitor, which is not necessarily
    /// the monitor that got split — on a multi-monitor desk the user may have been splitting a
    /// secondary display.
    /// </summary>
    [JsonPropertyName("previousPrimaryDevice")]
    public string PreviousPrimaryDevice { get; set; } = string.Empty;

    [JsonPropertyName("previousPrimaryFriendlyName")]
    public string PreviousPrimaryFriendlyName { get; set; } = string.Empty;

    /// <summary>Device name of the virtual monitor that Apply promoted to primary (segment 1).</summary>
    [JsonPropertyName("appliedPrimaryDevice")]
    public string AppliedPrimaryDevice { get; set; } = string.Empty;

    [JsonPropertyName("lastAppliedUtc")]
    public DateTime? LastAppliedUtc { get; set; }

    [JsonPropertyName("minimizeToTrayOnClose")]
    public bool MinimizeToTrayOnClose { get; set; } = true;
}

public sealed class SegmentSize
{
    [JsonPropertyName("width")] public int Width { get; set; }
    [JsonPropertyName("height")] public int Height { get; set; }
}
