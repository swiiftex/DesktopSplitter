using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Threading;
using DesktopSplitter.Interop;
using DesktopSplitter.Models;

namespace DesktopSplitter.Services;

public sealed record AdminFlowResult(bool Success, string Message)
{
    /// <summary>True when we did not even try, so the caller should fall back rather than report failure.</summary>
    public bool NotAttempted { get; init; }
}

/// <summary>
/// Drives Windows' own elevated helper for "Remove display from desktop":
///
///   SystemSettingsAdminFlows.exe SpecializeDisplay &lt;adapterLow&gt; &lt;adapterHigh&gt; &lt;targetId&gt; &lt;enable&gt; &lt;hash&gt;
///
/// This is the command line SettingsHandlers_PCDisplay.dll builds when the user flips the toggle.
/// Shelling out to it is deliberately preferred over crafting the private type -23 DisplayConfig
/// packet ourselves: Microsoft's binary owns the privileged part, and it is far less fragile
/// across Windows versions.
///
/// The 5th argument is a hash of the monitor's stable identity (see <see cref="MonitorIdentityHash"/>).
/// The exact recipe is not yet proven, so this path stays DISABLED until a captured real-world
/// hash has been reproduced offline and the winning recipe recorded in settings. Until then
/// <see cref="TryRun"/> reports <see cref="AdminFlowResult.NotAttempted"/> and the guided
/// Settings flow handles everything.
/// </summary>
public static class SystemSettingsAdminFlowsClient
{
    public const string ExecutableName = "SystemSettingsAdminFlows.exe";
    public const string Verb = "SpecializeDisplay";

    public static string ExecutablePath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), ExecutableName);

    public static bool IsAvailable => File.Exists(ExecutablePath);

    /// <summary>True when this exact monitor has a known-good identity hash.</summary>
    public static bool CanDrive(MonitorInfo monitor) => IsAvailable && SpecializationHashStore.HasHash(monitor);

    /// <summary>
    /// Builds the exact argument list Windows itself uses. Returns null unless a hash was
    /// captured for THIS monitor — never derived, never defaulted, never borrowed.
    /// </summary>
    public static IReadOnlyList<string>? BuildArguments(MonitorInfo monitor, bool enable)
    {
        ulong? hash = SpecializationHashStore.TryGet(monitor);
        if (hash is null || hash.Value == 0) return null;

        return BuildArguments(monitor, enable, hash.Value);
    }

    /// <summary>Builds the argument list for an explicit hash (used by the round-trip test).</summary>
    public static IReadOnlyList<string> BuildArguments(MonitorInfo monitor, bool enable, ulong hash)
        => new[]
        {
            Verb,
            monitor.AdapterIdLow.ToString(),
            monitor.AdapterIdHigh.ToString(),
            monitor.TargetId.ToString(),
            enable ? "1" : "0",
            hash.ToString(),
        };

    /// <summary>
    /// Runs the helper elevated (one UAC prompt) and waits for it. Returns NotAttempted when the
    /// hash recipe has not been proven yet, so the caller falls through to the guided flow.
    /// </summary>
    public static AdminFlowResult TryRun(
        MonitorInfo monitor, bool enable, IProgress<string> log, CancellationToken ct)
    {
        if (!IsAvailable)
        {
            return new AdminFlowResult(false, $"{ExecutablePath} not found.") { NotAttempted = true };
        }

        IReadOnlyList<string>? args = BuildArguments(monitor, enable);
        if (args is null)
        {
            return new AdminFlowResult(false,
                $"No identity hash has been learned for {monitor.DisplayLabel} yet, so the one-click " +
                "route is not available for it.")
            { NotAttempted = true };
        }

        var psi = new ProcessStartInfo
        {
            FileName = ExecutablePath,
            UseShellExecute = true,   // required for the runas verb
            Verb = "runas",
        };
        foreach (string a in args) psi.ArgumentList.Add(a);

        log.Report($"Running: {ExecutableName} {string.Join(" ", args)}   (elevated — expect a UAC prompt)");

        try
        {
            using Process? process = Process.Start(psi);
            if (process is null)
                return new AdminFlowResult(false, "The helper did not start.");

            if (!process.WaitForExit((int)TimeSpan.FromSeconds(60).TotalMilliseconds))
                return new AdminFlowResult(false, "The helper did not finish within 60 seconds.");

            return process.ExitCode == 0
                ? new AdminFlowResult(true,
                    $"{monitor.DisplayLabel} {(enable ? "removed from" : "restored to")} the desktop.")
                : new AdminFlowResult(false, $"The helper exited with code {process.ExitCode}.");
        }
        catch (Win32Exception ex) when (ex.NativeErrorCode == 1223)   // ERROR_CANCELLED
        {
            return new AdminFlowResult(false, "The elevation prompt was dismissed.");
        }
        catch (Exception ex)
        {
            return new AdminFlowResult(false, $"Could not run {ExecutableName}: {ex.Message}");
        }
    }
}
