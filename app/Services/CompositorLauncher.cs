using System.Diagnostics;
using System.IO;
using System.Threading;

namespace DesktopSplitter.Services;

public sealed class CompositorNotFoundException : Exception
{
    public CompositorNotFoundException(string message) : base(message) { }
}

/// <summary>
/// Locates, starts and stops dscomp.exe. Shutdown is signalled through the named
/// event from shared/DeskSplitProtocol.h.
/// </summary>
public static class CompositorLauncher
{
    public const string ExecutableName = "dscomp.exe";

    /// <summary>DESKSPLIT_COMPOSITOR_STOP_EVENT</summary>
    public const string StopEventName = @"Global\DeskSplitCompositorStop";

    /// <summary>
    /// The compositor signals this after its FIRST successful specialized scan-out. This — not
    /// WinRT UsageKind, which Windows reports unreliably — is the authoritative "hiding actually
    /// works" signal.
    /// </summary>
    public const string SpecializedLiveEventName = @"Global\DeskSplitSpecializedLive";

    /// <summary>Command-line switch that forces the specialized presenter for one run.</summary>
    public const string SpecializedArgument = "--specialized";

    /// <summary>
    /// Tells the compositor to keep polling until the target becomes specialized, then acquire —
    /// falling back to the window path if the wait expires. This is what lets the compositor be
    /// already waiting when the user flips Windows' "Remove display from desktop" toggle, so the
    /// panel is never left black with nothing drawing to it.
    /// </summary>
    public const string WaitForTargetArgument = "--wait-for-target";

    /// <summary>Acquired but no first frame within N seconds -> clean release and exit 20.</summary>
    public const string FrameDeadlineArgument = "--frame-deadline";

    /// <summary>Seconds the compositor waits for the display to become specialized.</summary>
    public const int WaitForTargetSeconds = 90;

    /// <summary>Seconds the compositor allows between acquisition and the first frame.</summary>
    public const int FrameDeadlineSeconds = 10;

    /// <summary>Outcome of waiting for the compositor to confirm specialized presentation.</summary>
    public enum SpecializedWait
    {
        /// <summary>The compositor scanned out through the specialized path.</summary>
        Live,

        /// <summary>The compositor exited before confirming.</summary>
        CompositorExited,

        /// <summary>Nothing was signalled inside the deadline.</summary>
        TimedOut,
    }

    /// <summary>
    /// The compositor MSBuild project outputs to compositor\build\x64\{Configuration}\dscomp.exe.
    /// Release is preferred; Debug is accepted so a dev build still launches.
    /// </summary>
    private static readonly string[] BuildConfigurations = { "Release", "Debug" };

    private const string CompositorOutputRoot = "compositor";

    private static readonly string[] RelativeProbes = BuildProbes();

    private static string[] BuildProbes()
    {
        var probes = new List<string>
        {
            ExecutableName,   // next to the app (deployed layout) — always tried first
        };

        // Walk up from the app's output directory to the repo root, checking both configurations.
        for (int up = 0; up <= 5; up++)
        {
            string prefix = string.Concat(Enumerable.Repeat(".." + Path.DirectorySeparatorChar, up));
            foreach (string configuration in BuildConfigurations)
                probes.Add(Path.Combine(prefix + CompositorOutputRoot, "build", "x64", configuration, ExecutableName));
        }
        return probes.ToArray();
    }

    /// <summary>
    /// The path shown to the user when the compositor cannot be located: the Release output
    /// under whichever ancestor directory actually contains a <c>compositor</c> folder, or —
    /// if there is none — the repo-root position implied by the standard
    /// <c>app\bin\{Configuration}\{tfm}</c> output layout.
    /// </summary>
    public static string ExpectedPath
    {
        get
        {
            var dir = new DirectoryInfo(BaseDirectory);
            for (int up = 0; up <= 5 && dir is not null; up++, dir = dir.Parent)
            {
                string candidate = Path.Combine(dir.FullName, CompositorOutputRoot);
                if (Directory.Exists(candidate))
                    return Path.Combine(candidate, "build", "x64", "Release", ExecutableName);
            }

            return Path.GetFullPath(Path.Combine(
                BaseDirectory, "..", "..", "..", "..", CompositorOutputRoot, "build", "x64", "Release", ExecutableName));
        }
    }

    private static string BaseDirectory => AppContext.BaseDirectory;

    public static bool TryFindExecutable(out string path)
    {
        foreach (string probe in RelativeProbes)
        {
            string candidate = Path.GetFullPath(Path.Combine(BaseDirectory, probe));
            if (File.Exists(candidate))
            {
                path = candidate;
                return true;
            }
        }
        path = string.Empty;
        return false;
    }

    public static bool IsRunning() => GetRunningProcesses().Length > 0;

    public static Process[] GetRunningProcesses()
    {
        try
        {
            return Process.GetProcessesByName(Path.GetFileNameWithoutExtension(ExecutableName));
        }
        catch
        {
            return Array.Empty<Process>();
        }
    }

    /// <summary>
    /// Waits for the compositor to signal <see cref="SpecializedLiveEventName"/>, while watching
    /// for it dying first. Any early exit is treated as failure regardless of the exit code, so
    /// this stays correct whatever code the compositor reserves for a post-acquisition failure;
    /// the code itself is logged.
    /// </summary>
    public static SpecializedWait WaitForSpecializedLive(
        Process compositor, TimeSpan timeout, CancellationToken ct, Action<string>? log = null)
    {
        DateTime deadline = DateTime.UtcNow + timeout;
        EventWaitHandle? live = null;

        try
        {
            while (DateTime.UtcNow < deadline)
            {
                ct.ThrowIfCancellationRequested();

                if (live is null &&
                    EventWaitHandle.TryOpenExisting(SpecializedLiveEventName, out EventWaitHandle? opened))
                {
                    live = opened;
                    log?.Invoke($"Opened {SpecializedLiveEventName}.");
                }

                if (live is not null && live.WaitOne(TimeSpan.Zero)) return SpecializedWait.Live;

                if (compositor.HasExited)
                {
                    log?.Invoke($"Compositor exited with code {compositor.ExitCode} before confirming " +
                                "specialized presentation.");
                    return SpecializedWait.CompositorExited;
                }

                Thread.Sleep(100);
            }
        }
        catch (UnauthorizedAccessException)
        {
            log?.Invoke($"WARNING: {SpecializedLiveEventName} exists but could not be opened (DACL).");
        }
        catch (WaitHandleCannotBeOpenedException)
        {
            // Never created — falls through to the timeout result.
        }
        finally
        {
            live?.Dispose();
        }

        return SpecializedWait.TimedOut;
    }

    /// <summary>Starts the compositor. Throws <see cref="CompositorNotFoundException"/> if missing.</summary>
    public static Process Start(params string[] arguments)
    {
        if (!TryFindExecutable(out string exe))
        {
            throw new CompositorNotFoundException(
                $"Compositor not found. Build it and place {ExecutableName} at:{Environment.NewLine}" +
                $"    {ExpectedPath}{Environment.NewLine}" +
                $"(or next to DesktopSplitter.exe in {BaseDirectory}).");
        }

        var psi = new ProcessStartInfo
        {
            FileName = exe,
            WorkingDirectory = Path.GetDirectoryName(exe) ?? BaseDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        foreach (string argument in arguments) psi.ArgumentList.Add(argument);

        Process? p = Process.Start(psi);
        if (p is null)
            throw new CompositorNotFoundException($"Failed to start '{exe}'.");
        return p;
    }

    /// <summary>Signals the stop event. Returns false when no compositor was listening.</summary>
    public static bool SignalStop()
    {
        try
        {
            if (EventWaitHandle.TryOpenExisting(StopEventName, out EventWaitHandle? handle) && handle is not null)
            {
                using (handle) { handle.Set(); }
                return true;
            }
        }
        catch (UnauthorizedAccessException)
        {
            // The event exists but this token cannot open it for modify access.
            return false;
        }
        catch (WaitHandleCannotBeOpenedException)
        {
            return false;
        }
        return false;
    }

    /// <summary>
    /// Signals the stop event and waits for every dscomp.exe to exit.
    /// Returns true if all processes are gone by the deadline.
    /// </summary>
    public static bool StopAndWait(TimeSpan timeout, Action<string>? log = null)
    {
        Process[] running = GetRunningProcesses();
        if (running.Length == 0)
        {
            log?.Invoke("Compositor is not running.");
            SignalStop();   // harmless; clears a stale signal state for the next run
            return true;
        }

        bool signalled = SignalStop();
        log?.Invoke(signalled
            ? $"Signalled {StopEventName}; waiting for the compositor to exit."
            : $"Could not open {StopEventName}; falling back to WM_CLOSE.");

        if (!signalled)
        {
            foreach (Process p in running)
            {
                try { p.CloseMainWindow(); } catch { /* best effort */ }
            }
        }

        DateTime deadline = DateTime.UtcNow + timeout;
        foreach (Process p in running)
        {
            try
            {
                int remaining = (int)Math.Max(0, (deadline - DateTime.UtcNow).TotalMilliseconds);
                if (!p.WaitForExit(remaining))
                {
                    log?.Invoke($"Compositor (pid {p.Id}) did not exit within the timeout — terminating it.");
                    try { p.Kill(entireProcessTree: true); p.WaitForExit(3000); } catch { /* best effort */ }
                }
            }
            catch (InvalidOperationException) { /* already gone */ }
            finally { p.Dispose(); }
        }

        return GetRunningProcesses().Length == 0;
    }
}
