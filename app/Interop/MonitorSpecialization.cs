using System.ComponentModel;
using System.Runtime.InteropServices;
using DesktopSplitter.Models;
using static DesktopSplitter.Interop.NativeMethods;

namespace DesktopSplitter.Interop;

/// <summary>The three defined bits of DISPLAYCONFIG_GET_MONITOR_SPECIALIZATION.value.</summary>
public readonly record struct SpecializationState(uint RawValue)
{
    /// <summary>Bit 0 — the display is specialized right now (off the desktop).</summary>
    public bool EnabledNow => (RawValue & 0x1) != 0;

    /// <summary>Bit 1 — this particular panel is eligible.</summary>
    public bool AvailableForMonitor => (RawValue & 0x2) != 0;

    /// <summary>Bit 2 — this Windows SKU allows it.</summary>
    public bool AvailableForSystem => (RawValue & 0x4) != 0;

    /// <summary>Eligible on both counts, so a SET has a chance of succeeding.</summary>
    public bool Available => AvailableForMonitor && AvailableForSystem;

    public override string ToString()
        => $"0x{RawValue:X} (enabled={(EnabledNow ? "YES" : "no")}, " +
           $"monitor={(AvailableForMonitor ? "YES" : "NO")}, system={(AvailableForSystem ? "YES" : "NO")})";
}

public sealed record SpecializationResult(bool Success, int ErrorCode, string Message)
{
    /// <summary>True when Windows refused for lack of rights — the feature must then be gated, never auto-elevated.</summary>
    public bool AccessDenied => ErrorCode == MonitorSpecialization.ERROR_ACCESS_DENIED;
}

/// <summary>
/// Monitor specialization via DisplayConfig types 12 (GET) and 13 (SET) — the same mechanism
/// behind Settings &gt; Advanced display &gt; "Remove display from desktop".
///
/// This replaces the retired EDID-override approach, which Microsoft documents as NOT a
/// supported way to designate a display specialized.
///
/// Two constraints that shape how callers must sequence things:
///   * Only a NON-PRIMARY display can be specialized, so promote another display to primary first.
///   * WinRT DisplayMonitor.UsageKind is NOT a reliable success signal — Windows has an open bug
///     where a correctly specialized display still reports Standard. The real signal is the
///     compositor actually acquiring and scanning out.
/// </summary>
public static class MonitorSpecialization
{
    internal const int ERROR_ACCESS_DENIED = 5;
    internal const int ERROR_INVALID_PARAMETER = 87;
    internal const int ERROR_NOT_SUPPORTED = 50;
    internal const int ERROR_GEN_FAILURE = 31;

    /// <summary>
    /// GUID_MONITOR_OVERRIDE_PSEUDO_SPECIALIZED, from ntddvdeo.h's "Monitor override types".
    /// "Pseudo specialized" is exactly this case: an ordinary panel that Windows is asked to
    /// treat as specialized, as opposed to one whose EDID genuinely declares it.
    /// (The sibling constant GUID_MONITOR_OVERRIDE_TEST_SPECIALIZED is for test use.)
    /// </summary>
    public static readonly Guid SpecializationType = new("F196C02F-F86F-4F9A-AA15-E9CEBDFE3B96");

    /// <summary>GUID_MONITOR_OVERRIDE_TEST_SPECIALIZED — not used, kept for reference.</summary>
    public static readonly Guid TestSpecializationType = new("0457E531-3CB9-4A07-83C1-A79146C64DB3");

    public static readonly Guid SpecializationSubType = Guid.Empty;

    public const string ApplicationName = "DesktopSplitter";

    /// <summary>Read-only probe. Never changes anything.</summary>
    public static bool TryGet(MonitorInfo monitor, out SpecializationState state, out int errorCode)
    {
        var packet = default(DISPLAYCONFIG_GET_MONITOR_SPECIALIZATION);
        packet.header = BuildHeader(monitor,
            DISPLAYCONFIG_DEVICE_INFO_GET_MONITOR_SPECIALIZATION,
            (uint)Marshal.SizeOf<DISPLAYCONFIG_GET_MONITOR_SPECIALIZATION>());

        errorCode = DisplayConfigGetDeviceInfo(ref packet);
        if (errorCode != ERROR_SUCCESS)
        {
            state = default;
            return false;
        }

        state = new SpecializationState(packet.value);
        return true;
    }

    /// <summary>Read-only probe that folds failure into an "unknown" state.</summary>
    public static SpecializationState? Get(MonitorInfo monitor)
        => TryGet(monitor, out SpecializationState state, out _) ? state : null;

    /// <summary>
    /// Turns specialization on or off for <paramref name="monitor"/>. The caller must have
    /// already ensured the monitor is not primary.
    /// </summary>
    public static SpecializationResult Set(MonitorInfo monitor, bool enabled)
    {
        var packet = default(DISPLAYCONFIG_SET_MONITOR_SPECIALIZATION);
        packet.header = BuildHeader(monitor,
            DISPLAYCONFIG_DEVICE_INFO_SET_MONITOR_SPECIALIZATION,
            (uint)Marshal.SizeOf<DISPLAYCONFIG_SET_MONITOR_SPECIALIZATION>());
        packet.value = enabled ? 1u : 0u;
        packet.specializationType = SpecializationType;
        packet.specializationSubType = SpecializationSubType;
        packet.specializationApplicationName = ApplicationName;

        int rc = DisplayConfigSetDeviceInfo(ref packet);
        if (rc == ERROR_SUCCESS)
        {
            return new SpecializationResult(true, rc,
                $"{monitor.DisplayLabel}: specialization {(enabled ? "ENABLED" : "disabled")}.");
        }

        return new SpecializationResult(false, rc, Explain(rc, monitor, enabled, packet));
    }

    /// <summary>
    /// Facts about the attempt, so a failure message never has to guess. Measured behaviour on
    /// Windows 11 26200 (ProfessionalWorkstation): an unelevated caller gets ERROR_ACCESS_DENIED
    /// for every well-formed request, on both physical and IddCx targets, regardless of the GUIDs
    /// or application name. A wrong header.size gives ERROR_INVALID_PARAMETER (87) or
    /// ERROR_INSUFFICIENT_BUFFER (122) instead, which is how we know the layout is right.
    /// </summary>
    private static string Explain(
        int rc, MonitorInfo monitor, bool enabled, DISPLAYCONFIG_SET_MONITOR_SPECIALIZATION packet)
    {
        string action = enabled ? "remove from the desktop" : "restore to the desktop";
        string what = $"Could not {action} {monitor.DisplayLabel} ({monitor.DeviceName}).";
        string parameters =
            $"Request sent: adapter={monitor.AdapterIdHigh:X8}-{monitor.AdapterIdLow:X8} " +
            $"target={monitor.TargetId}, size={packet.header.size}, value=0x{packet.value:X}, " +
            $"type={{{packet.specializationType}}}, subType={{{packet.specializationSubType}}}, " +
            $"app='{packet.specializationApplicationName}'.";

        string primaryNote = DescribePrimary(monitor);

        return rc switch
        {
            ERROR_ACCESS_DENIED =>
                $"{what} Windows denied access (ERROR_ACCESS_DENIED). Measured on this API: an " +
                "unelevated process is refused for every monitor and every parameter combination, " +
                "so this almost certainly needs administrator rights. DesktopSplitter will not " +
                $"elevate itself — use the Windows Settings route instead.{Environment.NewLine}{parameters}",

            ERROR_INVALID_PARAMETER =>
                $"{what} Windows rejected the parameters (ERROR_INVALID_PARAMETER). " +
                $"{primaryNote}{Environment.NewLine}{parameters}",

            ERROR_NOT_SUPPORTED =>
                $"{what} Not supported on this system (ERROR_NOT_SUPPORTED).{Environment.NewLine}{parameters}",

            ERROR_GEN_FAILURE =>
                $"{what} Windows reported a general failure (ERROR_GEN_FAILURE).{Environment.NewLine}{parameters}",

            _ => $"{what} {new Win32Exception(rc).Message} (error {rc}).{Environment.NewLine}{parameters}",
        };
    }

    /// <summary>
    /// States which display is actually primary, instead of guessing. Only a non-primary display
    /// can be specialized, so this is worth reporting — but only as a fact, never as a guess.
    /// </summary>
    private static string DescribePrimary(MonitorInfo monitor)
    {
        try
        {
            MonitorInfo? primary = DisplayConfig.Enumerate().FirstOrDefault(m => m.IsPrimary);
            if (primary is null) return "No display currently reports as primary.";

            return SameDevice(primary.DeviceName, monitor.DeviceName)
                ? $"This monitor IS currently the primary display ({primary.DeviceName}), and Windows " +
                  "only allows non-primary displays to be removed from the desktop."
                : $"This monitor is not primary — {primary.DisplayLabel} ({primary.DeviceName}) is — " +
                  "so the primary-display restriction is not the cause.";
        }
        catch (DisplayConfigException)
        {
            return "The current primary display could not be determined.";
        }
    }

    private static bool SameDevice(string a, string b)
        => string.Equals(a, b, StringComparison.OrdinalIgnoreCase);

    private static DISPLAYCONFIG_DEVICE_INFO_HEADER BuildHeader(MonitorInfo monitor, uint type, uint size)
        => new()
        {
            type = type,
            size = size,
            adapterId = new LUID { LowPart = monitor.AdapterIdLow, HighPart = monitor.AdapterIdHigh },
            id = monitor.TargetId,
        };
}
