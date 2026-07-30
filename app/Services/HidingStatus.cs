using DesktopSplitter.Interop;
using DesktopSplitter.Models;
using Microsoft.Win32;

namespace DesktopSplitter.Services;

/// <summary>State of a status row.</summary>
public enum CheckState
{
    /// <summary>Could not be determined.</summary>
    Unknown,

    /// <summary>A capability or setting that is satisfied.</summary>
    Ok,

    /// <summary>A capability or setting that is NOT satisfied.</summary>
    No,

    /// <summary>
    /// Purely informational — neither good nor bad. Used for "Currently hidden: no", which is the
    /// normal resting state and must not read as a problem.
    /// </summary>
    Info,
}

public sealed record StatusRow(string Label, CheckState State, string Detail);

/// <summary>Everything the display-hiding panel shows, gathered in one pass.</summary>
public sealed record HidingSnapshot(
    StatusRow SystemSupport,
    StatusRow MonitorSupport,
    StatusRow CurrentlySpecialized,
    StatusRow Intent,
    bool SplitActive,
    bool Available,
    bool EnabledNow,
    bool IntentOn)
{
    public IReadOnlyList<StatusRow> Rows => new[] { SystemSupport, MonitorSupport, CurrentlySpecialized, Intent };
}

/// <summary>
/// Read-only probes behind the "Display hiding" panel, built on DisplayConfig monitor
/// specialization (GET, type 12) — the supported mechanism that Settings &gt; Advanced display
/// uses. Nothing here changes display state.
/// </summary>
public static class HidingStatus
{
    private const string DeskSplitKey = @"SOFTWARE\DesktopSplitter";
    private const string HideFlagValue = "HidePhysicalDisplay";

    public static HidingSnapshot Query(MonitorInfo? selected)
    {
        bool intentOn = ReadIntent();
        StatusRow intent = intentOn
            ? new StatusRow("Hide during Apply", CheckState.Ok,
                "On — Apply will remove the physical monitor from the desktop once the split is up.")
            : new StatusRow("Hide during Apply", CheckState.No,
                "Off — the compositor just covers the physical monitor (the default).");

        if (selected is null) return Degenerate("No monitor selected.", intent, intentOn);

        if (!MonitorSpecialization.TryGet(selected, out SpecializationState state, out int rc))
        {
            string detail = $"DisplayConfig GET (type 12) failed with error {rc}" +
                            (rc == MonitorSpecialization.ERROR_INVALID_PARAMETER
                                ? " — this Windows build may not support monitor specialization."
                                : ".");
            return Degenerate(detail, intent, intentOn);
        }

        // Capability rows say what is POSSIBLE. The state row says what is HAPPENING. Keeping the
        // two visibly distinct matters: a green "supports hiding" was previously misread as
        // "already hiding".
        var system = new StatusRow("Windows can hide displays",
            state.AvailableForSystem ? CheckState.Ok : CheckState.No,
            state.AvailableForSystem
                ? "Yes — this Windows edition allows displays to be removed from the desktop."
                : "No — this Windows edition does not allow displays to be removed from the desktop.");

        var monitor = new StatusRow("This monitor can be hidden",
            state.AvailableForMonitor ? CheckState.Ok : CheckState.No,
            state.AvailableForMonitor
                ? $"Yes — {selected.DisplayLabel} is eligible (capability only; GET raw 0x{state.RawValue:X})."
                : $"No — {selected.DisplayLabel} is not eligible (GET raw 0x{state.RawValue:X}).");

        var current = new StatusRow("Hidden right now",
            state.EnabledNow ? CheckState.Ok : CheckState.Info,
            state.EnabledNow
                ? $"YES — {selected.DisplayLabel} is off the desktop right now."
                : $"No — {selected.DisplayLabel} is a normal desktop display right now.");

        return new HidingSnapshot(system, monitor, current, intent,
                                  IsSplitActive(), state.Available, state.EnabledNow, intentOn);
    }

    private static HidingSnapshot Degenerate(string detail, StatusRow intent, bool intentOn)
        => new(new StatusRow("Windows can hide displays", CheckState.Unknown, detail),
               new StatusRow("This monitor can be hidden", CheckState.Unknown, detail),
               new StatusRow("Hidden right now", CheckState.Unknown, detail),
               intent, IsSplitActive(), false, false, intentOn);

    /// <summary>A split must be reverted before hiding can be enabled or disabled.</summary>
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

    // ------------------------------------------------------------------ intent flag

    /// <summary>
    /// Reads the "hide during Apply" intent. HKLM wins when present (the installer or an admin
    /// may have set it, and it is what a standalone dscomp run reads); otherwise the per-user
    /// settings file, which never needs elevation.
    /// </summary>
    public static bool ReadIntent()
    {
        int? machine = ReadDword(Registry.LocalMachine, DeskSplitKey, HideFlagValue);
        if (machine is not null) return machine.Value != 0;
        return SettingsStore.Load()?.HidePhysicalDisplay ?? false;
    }

    public sealed record IntentWriteResult(bool Success, bool MachineWideWritten, string Message);

    /// <summary>
    /// Records the intent. The per-user copy always succeeds; the HKLM mirror is best-effort and
    /// its failure is reported, never worked around by elevating.
    /// </summary>
    public static IntentWriteResult WriteIntent(bool enabled)
    {
        AppSettings settings = SettingsStore.Load() ?? new AppSettings();
        settings.HidePhysicalDisplay = enabled;
        try
        {
            SettingsStore.Save(settings);
        }
        catch (Exception ex)
        {
            return new IntentWriteResult(false, false, "Could not save the setting: " + ex.Message);
        }

        try
        {
            using RegistryKey key = Registry.LocalMachine.CreateSubKey(DeskSplitKey, writable: true)
                                   ?? throw new UnauthorizedAccessException("key could not be opened");
            key.SetValue(HideFlagValue, enabled ? 1 : 0, RegistryValueKind.DWord);
            return new IntentWriteResult(true, true,
                $"Display hiding {(enabled ? "enabled" : "disabled")} (saved for this user and machine-wide).");
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or System.Security.SecurityException)
        {
            return new IntentWriteResult(true, false,
                $"Display hiding {(enabled ? "enabled" : "disabled")} for this user. " +
                $@"The machine-wide flag (HKLM\{DeskSplitKey}\{HideFlagValue}) needs administrator " +
                "rights and was left alone — DesktopSplitter passes --specialized to the compositor " +
                "itself, so this only matters if you run dscomp.exe standalone.");
        }
        catch (Exception ex)
        {
            return new IntentWriteResult(true, false,
                $"Display hiding {(enabled ? "enabled" : "disabled")} for this user; the machine-wide " +
                $"flag could not be written: {ex.Message}");
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
