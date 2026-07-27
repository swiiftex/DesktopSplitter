using System.Threading.Tasks;
using DesktopSplitter.Interop;
using DesktopSplitter.Models;
using Microsoft.Win32;

namespace DesktopSplitter.Services;

/// <summary>Tri-state for a status row: we could not tell / yes / no.</summary>
public enum CheckState
{
    Unknown,
    Ok,
    No,
}

public sealed record StatusRow(string Label, CheckState State, string Detail);

/// <summary>Everything the display-hiding panel shows, gathered in one pass.</summary>
public sealed record HidingSnapshot(
    StatusRow OsSupport,
    StatusRow MonitorHideable,
    StatusRow OverrideApplied,
    StatusRow FeatureFlag,
    bool SplitActive,
    bool HelperFound,
    string HelperPath)
{
    public IReadOnlyList<StatusRow> Rows => new[] { OsSupport, MonitorHideable, OverrideApplied, FeatureFlag };

    public bool OsSupported => OsSupport.State == CheckState.Ok;
    public bool OverrideIsOurs => OverrideApplied.State == CheckState.Ok;
    public bool FlagOn => FeatureFlag.State == CheckState.Ok;
}

/// <summary>
/// Read-only probes behind the "Display hiding" panel. Everything here is safe to call at any
/// time — no elevation, no device changes.
/// </summary>
public static class HidingStatus
{
    /// <summary>Editions where <c>DisplayManager</c> allows driving a specialized display.</summary>
    public static readonly string[] SupportedEditions =
    {
        "ProfessionalWorkstation", "Enterprise", "IoTEnterprise",
    };

    private const string WindowsNtKey = @"SOFTWARE\Microsoft\Windows NT\CurrentVersion";
    private const string DeskSplitKey = @"SOFTWARE\DesktopSplitter";
    private const string HideFlagValue = "HidePhysicalDisplay";

    public static async Task<HidingSnapshot> QueryAsync(MonitorInfo? selected)
    {
        StatusRow os = CheckOsEdition();
        StatusRow monitor = await CheckMonitorHideableAsync(selected).ConfigureAwait(false);
        StatusRow over = CheckOverrideApplied(selected);
        StatusRow flag = CheckFeatureFlag();

        bool splitActive = IsSplitActive();
        bool helperFound = EdidGuardClient.TryFindExecutable(out string helperPath);

        return new HidingSnapshot(os, monitor, over, flag, splitActive, helperFound,
                                  helperFound ? helperPath : EdidGuardClient.ExpectedPath);
    }

    // ------------------------------------------------------------------ individual checks

    public static StatusRow CheckOsEdition()
    {
        string edition = ReadString(Registry.LocalMachine, WindowsNtKey, "EditionID") ?? string.Empty;
        string build = ReadString(Registry.LocalMachine, WindowsNtKey, "CurrentBuildNumber") ?? "?";

        if (string.IsNullOrEmpty(edition))
            return new StatusRow("Windows edition", CheckState.Unknown, "Could not read EditionID.");

        bool supported = SupportedEditions.Contains(edition, StringComparer.OrdinalIgnoreCase);
        return new StatusRow(
            "Windows edition",
            supported ? CheckState.Ok : CheckState.No,
            supported
                ? $"{edition} (build {build}) — specialized displays allowed."
                : $"{edition} (build {build}) — needs one of {string.Join(", ", SupportedEditions)}.");
    }

    private static async Task<StatusRow> CheckMonitorHideableAsync(MonitorInfo? selected)
    {
        if (selected is null)
            return new StatusRow("Monitor is hideable", CheckState.Unknown, "No monitor selected.");

        IReadOnlyDictionary<string, MonitorUsage> map = await DisplayUsage.BuildMapAsync().ConfigureAwait(false);
        MonitorUsage usage = DisplayUsage.Lookup(map, selected.MonitorDevicePath);

        return usage switch
        {
            MonitorUsage.SpecialPurpose => new StatusRow("Monitor is hideable", CheckState.Ok,
                $"{selected.DisplayLabel}: UsageKind = SpecialPurpose — already out of the desktop."),
            MonitorUsage.Standard => new StatusRow("Monitor is hideable", CheckState.No,
                $"{selected.DisplayLabel}: UsageKind = Standard — enable hiding to mark it specialized."),
            _ => new StatusRow("Monitor is hideable", CheckState.Unknown,
                $"{selected.DisplayLabel}: UsageKind could not be read."),
        };
    }

    private static StatusRow CheckOverrideApplied(MonitorInfo? selected)
    {
        if (selected is null)
            return new StatusRow("DesktopSplitter override", CheckState.Unknown, "No monitor selected.");

        try
        {
            IReadOnlyDictionary<string, EdidReader.DevnodeEdidState> map = EdidReader.BuildOverrideStateMap();
            EdidReader.DevnodeEdidState? state = EdidReader.LookupOverrideState(map, selected.MonitorDevicePath);

            if (state is null)
                return new StatusRow("DesktopSplitter override", CheckState.Unknown,
                    "Could not read the monitor's Device Parameters key.");

            if (state.HasDeskSplitBackup)
                return new StatusRow("DesktopSplitter override", CheckState.Ok,
                    "Applied by DesktopSplitter (EDID_OVERRIDE_DSBACKUP present).");

            return new StatusRow("DesktopSplitter override", CheckState.No,
                state.HasOverride
                    ? "An EDID_OVERRIDE exists but was not written by DesktopSplitter — leaving it alone."
                    : "Not applied.");
        }
        catch (Exception ex)
        {
            return new StatusRow("DesktopSplitter override", CheckState.Unknown, "Read failed: " + ex.Message);
        }
    }

    public static StatusRow CheckFeatureFlag()
    {
        int? value = ReadDword(Registry.LocalMachine, DeskSplitKey, HideFlagValue);
        return value switch
        {
            null => new StatusRow("HidePhysicalDisplay flag", CheckState.No,
                $@"Not set (HKLM\{DeskSplitKey}\{HideFlagValue}) — the compositor uses the window path."),
            0 => new StatusRow("HidePhysicalDisplay flag", CheckState.No,
                "Set to 0 — the compositor uses the window path."),
            _ => new StatusRow("HidePhysicalDisplay flag", CheckState.Ok,
                $"Set to {value} — the compositor will try the specialized presenter."),
        };
    }

    /// <summary>A split must be reverted before the monitor's EDID can be messed with.</summary>
    public static bool IsSplitActive()
    {
        try
        {
            if (DisplayConfig.Enumerate().Any(m => m.IsDeskSplitVirtual)) return true;
        }
        catch (DisplayConfigException)
        {
            // Fall through to the compositor check.
        }
        return CompositorLauncher.IsRunning();
    }

    // ------------------------------------------------------------------ registry helpers

    private static string? ReadString(RegistryKey root, string subKey, string name)
    {
        try
        {
            using RegistryKey? key = root.OpenSubKey(subKey);
            return key?.GetValue(name) as string;
        }
        catch
        {
            return null;
        }
    }

    private static int? ReadDword(RegistryKey root, string subKey, string name)
    {
        try
        {
            using RegistryKey? key = root.OpenSubKey(subKey);
            return key?.GetValue(name) is int value ? value : null;
        }
        catch
        {
            return null;
        }
    }
}
