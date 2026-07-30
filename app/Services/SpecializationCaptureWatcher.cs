using System.Diagnostics;
using System.Threading;
using DesktopSplitter.Interop;
using DesktopSplitter.Models;

namespace DesktopSplitter.Services;

/// <summary>One observed SystemSettingsAdminFlows.exe SpecializeDisplay invocation.</summary>
public sealed record SpecializeDisplayInvocation(
    uint AdapterLow, int AdapterHigh, uint TargetId, bool Enable, ulong Hash, string CommandLine)
{
    /// <summary>True when this invocation addresses exactly the given monitor.</summary>
    public bool Matches(MonitorInfo monitor)
        => AdapterLow == monitor.AdapterIdLow
           && AdapterHigh == monitor.AdapterIdHigh
           && TargetId == monitor.TargetId;
}

/// <summary>
/// Watches for Windows' own elevated helper while the user works the Settings toggle, so the app
/// can learn that monitor's identity hash and drive the same command line itself next time.
///
/// The helper is short-lived, hence the tight poll. Reading its command line requires elevation
/// (it runs as administrator); when we cannot, <see cref="ElevationRequired"/> is set so the UI
/// can say so plainly instead of silently learning nothing. The app never self-elevates for this.
/// </summary>
public sealed class SpecializationCaptureWatcher : IDisposable
{
    private const string HelperProcessName = "SystemSettingsAdminFlows";
    private const string Verb = "SpecializeDisplay";

    private readonly MonitorInfo _monitor;
    private readonly IProgress<string>? _log;
    private readonly CancellationTokenSource _stop = new();
    private readonly HashSet<int> _seen = new();
    private Thread? _thread;

    public SpecializationCaptureWatcher(MonitorInfo monitor, IProgress<string>? log = null)
    {
        _monitor = monitor;
        _log = log;
    }

    /// <summary>The invocation captured for our monitor, if any.</summary>
    public SpecializeDisplayInvocation? Captured { get; private set; }

    /// <summary>True when the helper ran but we lacked the rights to read its command line.</summary>
    public bool ElevationRequired { get; private set; }

    public void Start()
    {
        if (_thread is not null) return;
        _thread = new Thread(Loop) { IsBackground = true, Name = "DeskSplit specialization capture" };
        _thread.Start();
    }

    /// <summary>Stops watching and returns whatever was captured.</summary>
    public SpecializeDisplayInvocation? StopAndCollect()
    {
        try { _stop.Cancel(); } catch (ObjectDisposedException) { }
        try { _thread?.Join(TimeSpan.FromSeconds(2)); } catch { /* best effort */ }
        _thread = null;
        return Captured;
    }

    private void Loop()
    {
        bool canRead = ProcessCommandLine.IsElevated();

        while (!_stop.IsCancellationRequested)
        {
            try
            {
                foreach (Process process in Process.GetProcessesByName(HelperProcessName))
                {
                    using (process)
                    {
                        if (!_seen.Add(process.Id)) continue;

                        if (!canRead)
                        {
                            ElevationRequired = true;
                            continue;
                        }

                        string? commandLine = ProcessCommandLine.TryRead(process.Id);
                        if (commandLine is null)
                        {
                            ElevationRequired = true;
                            continue;
                        }

                        SpecializeDisplayInvocation? parsed = Parse(commandLine);
                        if (parsed is null) continue;

                        if (!parsed.Matches(_monitor))
                        {
                            _log?.Report($"Observed a display-specialization command for a different " +
                                         $"monitor (target {parsed.TargetId}); ignoring it.");
                            continue;
                        }

                        Captured = parsed;
                        _log?.Report($"Observed Windows' own command for {_monitor.DisplayLabel}: " +
                                     $"hash {parsed.Hash}.");
                    }
                }
            }
            catch
            {
                // Enumeration races are expected while processes come and go.
            }

            _stop.Token.WaitHandle.WaitOne(15);
        }
    }

    /// <summary>
    /// Parses <c>... SpecializeDisplay &lt;adapterLow&gt; &lt;adapterHigh&gt; &lt;targetId&gt;
    /// &lt;enable&gt; &lt;hash&gt;</c>. Returns null for anything that does not match exactly.
    /// </summary>
    public static SpecializeDisplayInvocation? Parse(string commandLine)
    {
        string[] parts = ProcessCommandLine.SplitArguments(commandLine);
        int verb = Array.FindIndex(parts, p => p.Equals(Verb, StringComparison.OrdinalIgnoreCase));
        if (verb < 0 || parts.Length < verb + 6) return null;

        if (!uint.TryParse(parts[verb + 1], out uint adapterLow)) return null;
        if (!int.TryParse(parts[verb + 2], out int adapterHigh)) return null;
        if (!uint.TryParse(parts[verb + 3], out uint targetId)) return null;
        if (!int.TryParse(parts[verb + 4], out int enable)) return null;
        if (!ulong.TryParse(parts[verb + 5], out ulong hash)) return null;
        if (hash == 0) return null;

        return new SpecializeDisplayInvocation(adapterLow, adapterHigh, targetId, enable != 0, hash, commandLine);
    }

    public void Dispose()
    {
        StopAndCollect();
        _stop.Dispose();
    }
}
