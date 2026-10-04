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

    /// <summary>
    /// User-scope intent: try to remove the physical monitor from the desktop during Apply.
    /// Mirrored to HKLM when we have rights (that copy is what a standalone dscomp run reads);
    /// the app itself passes --specialized so it never depends on the HKLM write succeeding.
    /// </summary>
    [JsonPropertyName("hidePhysicalDisplay")]
    public bool HidePhysicalDisplay { get; set; }

    /// <summary>
    /// The device we most recently turned monitor specialization ON for, so Revert — and
    /// crash recovery at next startup — knows exactly what to undo. Empty when we own nothing.
    /// </summary>
    [JsonPropertyName("specializedDevice")]
    public string SpecializedDevice { get; set; } = string.Empty;

    /// <summary>
    /// The MonitorIdentityHash recipe id proven to reproduce a real captured
    /// SystemSettingsAdminFlows.exe hash. Still unknown, so normally empty — learned hashes
    /// (below) are what actually drive the automatic route today.
    /// </summary>
    [JsonPropertyName("specializationHashRecipe")]
    public string SpecializationHashRecipe { get; set; } = string.Empty;

    /// <summary>
    /// Monitor identity hashes observed from real SystemSettingsAdminFlows.exe invocations,
    /// keyed by the monitor's device interface path (compared case-insensitively).
    ///
    /// A hash is only ever valid for the exact monitor it was captured from: passing the wrong
    /// one risks specializing a display in a state the Settings UI cannot undo. Never guess,
    /// never reuse another monitor's value.
    /// </summary>
    [JsonPropertyName("specializationHashes")]
    public Dictionary<string, string> SpecializationHashes { get; set; } = new();

    [JsonPropertyName("lastAppliedUtc")]
    public DateTime? LastAppliedUtc { get; set; }

    [JsonPropertyName("minimizeToTrayOnClose")]
    public bool MinimizeToTrayOnClose { get; set; } = true;

    /// <summary>Mirrors the HKCU Run entry. The registry stays the source of truth.</summary>
    [JsonPropertyName("startWithWindows")]
    public bool StartWithWindows { get; set; }

    /// <summary>Re-apply the saved configuration automatically when the app starts.</summary>
    [JsonPropertyName("applyOnStartup")]
    public bool ApplyOnStartup { get; set; }

    /// <summary>
    /// True while a split is applied; cleared when the user reverts deliberately. This is what
    /// separates "we were still split when we exited" from "they turned it off on purpose", so
    /// startup never resurrects something the user switched off.
    /// </summary>
    [JsonPropertyName("splitActiveOnExit")]
    public bool SplitActiveOnExit { get; set; }
}

public sealed class SegmentSize
{
    [JsonPropertyName("width")] public int Width { get; set; }
    [JsonPropertyName("height")] public int Height { get; set; }
}
