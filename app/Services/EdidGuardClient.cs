using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace DesktopSplitter.Services;

/// <summary>How a guarded EDID operation ended.</summary>
public enum GuardOutcome
{
    /// <summary>Exit 0 — the user pressed Keep; HidePhysicalDisplay is now 1.</summary>
    Kept,

    /// <summary>Exit 10 — the user pressed Revert now.</summary>
    RevertedByUser,

    /// <summary>Exit 11 — nobody answered in time, the helper reverted itself.</summary>
    AutoReverted,

    /// <summary>Exit 4 — the devnode restart failed, so the helper reverted itself.</summary>
    RestartFailedReverted,

    /// <summary>Exit 3 — the override could not be applied at all.</summary>
    ApplyFailed,

    /// <summary>Exit 2 — the helper was not running elevated.</summary>
    NotElevated,

    /// <summary>Exit 5 — the helper rejected our command line.</summary>
    BadArgs,

    /// <summary>The helper never signalled Armed.</summary>
    NeverArmed,

    /// <summary>The user dismissed the UAC prompt.</summary>
    LaunchCancelled,

    /// <summary>The helper could not be started.</summary>
    LaunchFailed,

    /// <summary>edidoverride.exe was not found.</summary>
    HelperMissing,

    /// <summary>The helper exited with a code we do not recognise.</summary>
    UnknownExitCode,
}

public sealed record GuardResult(GuardOutcome Outcome, int ExitCode, string Message)
{
    public bool Succeeded => Outcome == GuardOutcome.Kept;

    /// <summary>True when the display is known to be back in its original state.</summary>
    public bool DisplayRestored => Outcome is GuardOutcome.RevertedByUser
                                             or GuardOutcome.AutoReverted
                                             or GuardOutcome.RestartFailedReverted;
}

/// <summary>
/// Drives one run of <c>edidoverride.exe guarded-apply</c> / <c>guarded-revert</c>.
///
/// The elevated helper is the authority on the timeout: it applies the override, signals
/// <c>Global\DeskSplitEdidArmed</c>, then waits for Keep or Revert and auto-reverts if neither
/// arrives. This class only mirrors that in the UI and relays the user's answer, so a hung or
/// killed control app can never leave the display stranded.
/// </summary>
public sealed class EdidGuardSession
{
    public const string ArmedEventName = @"Global\DeskSplitEdidArmed";
    public const string KeepEventName = @"Global\DeskSplitEdidKeep";
    public const string RevertEventName = @"Global\DeskSplitEdidRevert";

    /// <summary>How long we will wait for the helper to signal Armed.</summary>
    public static readonly TimeSpan ArmTimeout = TimeSpan.FromSeconds(60);

    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(100);

    private readonly object _gate = new();
    private EventWaitHandle? _keep;
    private EventWaitHandle? _revert;
    private bool _answered;

    /// <summary>Raised once the helper reports the override is live. Argument is the timeout in seconds.</summary>
    public event Action<int>? Armed;

    /// <summary>Captured stdout/stderr — only populated for dry runs (elevated runs cannot be redirected).</summary>
    public string Output => _output.ToString();
    private readonly StringBuilder _output = new();

    /// <summary>Tells the helper to keep the change. Safe to call more than once.</summary>
    public bool RequestKeep() => Answer(_keep, "keep");

    /// <summary>Tells the helper to undo the change immediately.</summary>
    public bool RequestRevert() => Answer(_revert, "revert");

    private bool Answer(EventWaitHandle? handle, string which)
    {
        lock (_gate)
        {
            if (_answered || handle is null) return false;
            _answered = true;
        }
        try
        {
            handle.Set();
            return true;
        }
        catch (ObjectDisposedException)
        {
            return false;
        }
    }

    /// <summary>
    /// Runs <c>guarded-apply</c>. With <paramref name="dryRun"/> the helper walks the entire
    /// sequence — including the Armed/Keep/Revert dance and the exit codes — without changing
    /// anything, and needs no elevation.
    /// </summary>
    public async Task<GuardResult> ApplyAsync(
        string deviceName, bool dryRun, int timeoutSeconds,
        IProgress<string> log, CancellationToken ct, string? executableOverride = null)
    {
        var args = new List<string> { "guarded-apply", deviceName, "--timeout-seconds", timeoutSeconds.ToString() };
        args.Add(dryRun ? "--dry-run" : "--yes");
        return await RunAsync(args, elevated: !dryRun, expectArmed: true, timeoutSeconds, log, ct, executableOverride)
            .ConfigureAwait(false);
    }

    /// <summary>Runs <c>guarded-revert</c>, which needs no confirmation dance.</summary>
    public async Task<GuardResult> RevertAsync(
        string deviceName, bool dryRun,
        IProgress<string> log, CancellationToken ct, string? executableOverride = null)
    {
        var args = new List<string> { "guarded-revert", deviceName };
        args.Add(dryRun ? "--dry-run" : "--yes");
        return await RunAsync(args, elevated: !dryRun, expectArmed: false, 0, log, ct, executableOverride)
            .ConfigureAwait(false);
    }

    private async Task<GuardResult> RunAsync(
        IReadOnlyList<string> args, bool elevated, bool expectArmed, int timeoutSeconds,
        IProgress<string> log, CancellationToken ct, string? executableOverride)
    {
        string exe = executableOverride ?? string.Empty;
        if (string.IsNullOrEmpty(exe) && !EdidGuardClient.TryFindExecutable(out exe))
        {
            return new GuardResult(GuardOutcome.HelperMissing, -1,
                $"edidoverride.exe not found. Expected at:{Environment.NewLine}    {EdidGuardClient.ExpectedPath}");
        }

        var psi = new ProcessStartInfo { FileName = exe };
        foreach (string a in args) psi.ArgumentList.Add(a);
        psi.WorkingDirectory = Path.GetDirectoryName(exe) ?? AppContext.BaseDirectory;

        if (elevated)
        {
            // ShellExecute is the only way to get the UAC prompt; it rules out redirection,
            // which is why the helper also writes %ProgramData%\DesktopSplitter\edid-guard.log.
            psi.UseShellExecute = true;
            psi.Verb = "runas";
        }
        else
        {
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
        }

        log.Report($"Launching: {Path.GetFileName(exe)} {string.Join(" ", args)}" +
                   (elevated ? "   (elevated — expect a UAC prompt)" : "   (no elevation needed)"));

        Process process;
        try
        {
            Process? started = Process.Start(psi);
            if (started is null)
                return new GuardResult(GuardOutcome.LaunchFailed, -1, "The helper process did not start.");
            process = started;
        }
        catch (Win32Exception ex) when (ex.NativeErrorCode == 1223)   // ERROR_CANCELLED
        {
            return new GuardResult(GuardOutcome.LaunchCancelled, -1,
                "The elevation prompt was dismissed — nothing was changed.");
        }
        catch (Exception ex)
        {
            return new GuardResult(GuardOutcome.LaunchFailed, -1, $"Could not start the helper: {ex.Message}");
        }

        try
        {
            using (process)
            {
            if (!elevated) StartCapturing(process, log);

            if (expectArmed)
            {
                bool armed = await WaitForArmedAsync(process, log, ct).ConfigureAwait(false);
                if (!armed)
                {
                    // The helper may still be mid-sequence; let it finish (it self-reverts)
                    // before deciding what happened.
                    await WaitForExitAsync(process, ArmTimeout, ct).ConfigureAwait(false);
                    if (!process.HasExited)
                    {
                        return new GuardResult(GuardOutcome.NeverArmed, -1,
                            "The helper never reported that the change was applied. Check " +
                            $"{EdidGuardClient.LogPath}.");
                    }
                    return Interpret(process.ExitCode, armedSeen: false);
                }

                OpenAnswerEvents(log);
                log.Report($"Helper armed — {timeoutSeconds}s to keep or revert.");
                Armed?.Invoke(timeoutSeconds);
            }

            await WaitForExitAsync(process, Timeout.InfiniteTimeSpan, ct).ConfigureAwait(false);
            GuardResult result = Interpret(process.ExitCode, armedSeen: expectArmed);
            log.Report(result.Message);
            return result;
            }
        }
        finally
        {
            DisposeEvents();
        }
    }

    private void StartCapturing(Process process, IProgress<string> log)
    {
        process.OutputDataReceived += (_, e) =>
        {
            if (e.Data is null) return;
            lock (_output) _output.AppendLine(e.Data);
            log.Report("  helper: " + e.Data);
        };
        process.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is null) return;
            lock (_output) _output.AppendLine(e.Data);
            log.Report("  helper! " + e.Data);
        };
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
    }

    /// <summary>
    /// Polls for the Armed event, which only exists once the elevated side creates it, while
    /// watching for the helper dying early (bad args, not elevated, apply failed).
    /// </summary>
    private static async Task<bool> WaitForArmedAsync(Process process, IProgress<string> log, CancellationToken ct)
    {
        DateTime deadline = DateTime.UtcNow + ArmTimeout;
        EventWaitHandle? armed = null;

        try
        {
            while (DateTime.UtcNow < deadline)
            {
                ct.ThrowIfCancellationRequested();

                if (armed is null &&
                    EventWaitHandle.TryOpenExisting(ArmedEventName, out EventWaitHandle? opened))
                {
                    armed = opened;
                    log.Report($"Opened {ArmedEventName}.");
                }

                if (armed is not null && armed.WaitOne(TimeSpan.Zero)) return true;
                if (process.HasExited) return false;

                await Task.Delay(PollInterval, ct).ConfigureAwait(false);
            }
        }
        catch (UnauthorizedAccessException)
        {
            log.Report($"WARNING: {ArmedEventName} exists but could not be opened (DACL).");
        }
        catch (WaitHandleCannotBeOpenedException)
        {
            // Never created — falls through to the timeout result.
        }
        finally
        {
            armed?.Dispose();
        }

        return false;
    }

    private void OpenAnswerEvents(IProgress<string> log)
    {
        lock (_gate)
        {
            _keep = TryOpen(KeepEventName, log);
            _revert = TryOpen(RevertEventName, log);
        }
    }

    private static EventWaitHandle? TryOpen(string name, IProgress<string> log)
    {
        try
        {
            if (EventWaitHandle.TryOpenExisting(name, out EventWaitHandle? handle)) return handle;
            log.Report($"WARNING: {name} does not exist yet.");
        }
        catch (Exception ex)
        {
            log.Report($"WARNING: could not open {name}: {ex.Message}");
        }
        return null;
    }

    private void DisposeEvents()
    {
        lock (_gate)
        {
            _keep?.Dispose();
            _revert?.Dispose();
            _keep = null;
            _revert = null;
        }
    }

    private static async Task WaitForExitAsync(Process process, TimeSpan timeout, CancellationToken ct)
    {
        try
        {
            if (timeout == Timeout.InfiniteTimeSpan)
            {
                await process.WaitForExitAsync(ct).ConfigureAwait(false);
                return;
            }

            using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            cts.CancelAfter(timeout);
            await process.WaitForExitAsync(cts.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            // Timed out waiting; caller inspects HasExited.
        }
    }

    /// <summary>Maps the helper's exit code onto an outcome. Public so the probe can assert it.</summary>
    public static GuardResult Interpret(int exitCode, bool armedSeen) => exitCode switch
    {
        0 => new GuardResult(GuardOutcome.Kept, 0,
            armedSeen
                ? "Kept. The display override is active and HidePhysicalDisplay is set."
                : "Done. The display override was removed and HidePhysicalDisplay cleared."),
        10 => new GuardResult(GuardOutcome.RevertedByUser, 10,
            "Reverted at your request — the display is back to how it was."),
        11 => new GuardResult(GuardOutcome.AutoReverted, 11,
            "No answer within the timeout, so the change reverted itself automatically."),
        4 => new GuardResult(GuardOutcome.RestartFailedReverted, 4,
            "The monitor could not be restarted, so the override was rolled back. Nothing changed."),
        3 => new GuardResult(GuardOutcome.ApplyFailed, 3,
            "The EDID override could not be applied. Nothing changed."),
        2 => new GuardResult(GuardOutcome.NotElevated, 2,
            "The helper needs administrator rights."),
        5 => new GuardResult(GuardOutcome.BadArgs, 5,
            "The helper rejected the command line — the app and edidoverride.exe are probably out of step."),
        _ => new GuardResult(GuardOutcome.UnknownExitCode, exitCode,
            $"The helper exited with an unexpected code ({exitCode}). See {EdidGuardClient.LogPath}."),
    };
}

/// <summary>Locates edidoverride.exe next to the app, or in its build tree.</summary>
public static class EdidGuardClient
{
    public const string ExecutableName = "edidoverride.exe";

    private const string SourceRoot = "installer";
    private const string ProjectFolder = "edid-override";

    private static readonly string[] BuildConfigurations = { "Release", "Debug" };

    private static readonly string[] RelativeProbes = BuildProbes();

    private static string[] BuildProbes()
    {
        var probes = new List<string> { ExecutableName };   // installed layout: next to the app
        for (int up = 0; up <= 5; up++)
        {
            string prefix = string.Concat(Enumerable.Repeat(".." + Path.DirectorySeparatorChar, up));
            foreach (string configuration in BuildConfigurations)
            {
                probes.Add(Path.Combine(prefix + SourceRoot, ProjectFolder,
                                        "build", "x64", configuration, ExecutableName));
            }
        }
        return probes.ToArray();
    }

    /// <summary>The step log the elevated helper writes.</summary>
    public static string LogPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
        "DesktopSplitter", "edid-guard.log");

    public static string ExpectedPath
    {
        get
        {
            var dir = new DirectoryInfo(AppContext.BaseDirectory);
            for (int up = 0; up <= 5 && dir is not null; up++, dir = dir.Parent)
            {
                string candidate = Path.Combine(dir.FullName, SourceRoot, ProjectFolder);
                if (Directory.Exists(candidate))
                    return Path.Combine(candidate, "build", "x64", "Release", ExecutableName);
            }
            return Path.Combine(AppContext.BaseDirectory, ExecutableName);
        }
    }

    public static bool TryFindExecutable(out string path)
    {
        foreach (string probe in RelativeProbes)
        {
            string candidate = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, probe));
            if (File.Exists(candidate))
            {
                path = candidate;
                return true;
            }
        }
        path = string.Empty;
        return false;
    }
}
