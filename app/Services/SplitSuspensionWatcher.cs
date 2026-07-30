using System.Threading;
using DesktopSplitter.Models;

namespace DesktopSplitter.Services;

/// <summary>What the compositor is currently asking for.</summary>
public enum SuspensionRequest
{
    /// <summary>Neither event could be opened — the compositor probably is not running.</summary>
    Unknown,

    /// <summary>The split should be active: segment 1 primary.</summary>
    Active,

    /// <summary>Secure desktop / lock / manual toggle: hand primary back to the physical monitor.</summary>
    Suspended,
}

/// <summary>
/// Watches the compositor's suspend/resume events and hands the PRIMARY display back and forth.
///
/// Windows draws UAC prompts, Ctrl+Alt+Del and the lock screen on the primary display. Under a
/// split that is a virtual monitor which only exists through the compositor, so those screens have
/// nowhere real to appear. While suspended we put primary back on the physical monitor; the
/// virtual monitors and the compositor are deliberately left alone so the transition is fast and
/// reversible.
///
/// The events are manual-reset and mutually exclusive, so this is written as LEVEL-triggered
/// reconciliation rather than edge-triggered handling: every wake-up re-reads the desired state
/// and moves towards it. That makes repeated signals idempotent for free, and lets the watcher
/// recover from transitions it missed while an Apply held the display gate.
/// </summary>
public sealed class SplitSuspensionWatcher : IDisposable
{
    public const string SuspendEventName = "DeskSplitSuspendRequest";
    public const string ResumeEventName = "DeskSplitResumeRequest";

    /// <summary>Periodic re-check, so a missed or racing transition self-heals.</summary>
    private static readonly TimeSpan ReconcilePeriod = TimeSpan.FromSeconds(2);

    private readonly IProgress<string> _log;
    private readonly ManualResetEventSlim _stop = new(false);
    private readonly object _stateGate = new();

    private Thread? _thread;
    private EventWaitHandle? _suspend;
    private EventWaitHandle? _resume;

    /// <summary>Device that should be primary while the split is active (segment 1's virtual monitor).</summary>
    private string _activePrimaryDevice = string.Empty;

    /// <summary>Device that should be primary while suspended (the physical monitor).</summary>
    private string _suspendedPrimaryDevice = string.Empty;

    public SplitSuspensionWatcher(IProgress<string> log) => _log = log;

    /// <summary>True while primary has been handed back to the physical monitor.</summary>
    public bool IsSuspended { get; private set; }

    public bool IsRunning => _thread is not null;

    /// <summary>
    /// Starts watching for a live split. <paramref name="activePrimaryDevice"/> is segment 1's
    /// virtual monitor, <paramref name="suspendedPrimaryDevice"/> the physical one.
    /// </summary>
    public void Start(string activePrimaryDevice, string suspendedPrimaryDevice)
    {
        lock (_stateGate)
        {
            _activePrimaryDevice = activePrimaryDevice ?? string.Empty;
            _suspendedPrimaryDevice = suspendedPrimaryDevice ?? string.Empty;

            if (_thread is not null) return;
            if (string.IsNullOrEmpty(_activePrimaryDevice) || string.IsNullOrEmpty(_suspendedPrimaryDevice))
            {
                _log.Report("Suspend/resume watcher not started: the primary displays are not known.");
                return;
            }

            _stop.Reset();
            IsSuspended = false;
            _thread = new Thread(Loop) { IsBackground = true, Name = "DeskSplit suspend watcher" };
            _thread.Start();
            _log.Report("Watching for lock / UAC transitions (primary will move to " +
                        $"{_suspendedPrimaryDevice} while they are on screen).");
        }
    }

    /// <summary>
    /// Stops watching. <paramref name="restorePrimary"/> is false when the caller is about to tear
    /// the split down anyway — Revert restores primary itself, and doing it twice just adds churn.
    /// </summary>
    public void Stop(bool restorePrimary)
    {
        Thread? thread;
        lock (_stateGate)
        {
            thread = _thread;
            _thread = null;
        }
        if (thread is null) return;

        _stop.Set();
        try { thread.Join(TimeSpan.FromSeconds(3)); } catch { /* best effort */ }

        if (restorePrimary && IsSuspended)
        {
            _log.Report("Restoring the split's primary display before stopping the watcher.");
            MoveTo(_activePrimaryDevice, "segment 1");
        }

        IsSuspended = false;
        CloseHandles();
        _log.Report("Stopped watching for lock / UAC transitions.");
    }

    /// <summary>
    /// Reads the desired state without changing anything. Returns Unknown when neither event
    /// exists (the compositor is not running).
    /// </summary>
    public SuspensionRequest ReadDesiredState()
    {
        EnsureHandles();
        bool suspend = _suspend?.WaitOne(TimeSpan.Zero) ?? false;
        bool resume = _resume?.WaitOne(TimeSpan.Zero) ?? false;

        if (suspend && !resume) return SuspensionRequest.Suspended;
        if (resume && !suspend) return SuspensionRequest.Active;
        if (_suspend is null && _resume is null) return SuspensionRequest.Unknown;

        // Both or neither: the compositor guarantees exclusivity, so this is a transient race.
        // Treat it as "active" — the safe direction, since it never strands us suspended.
        return SuspensionRequest.Active;
    }

    private void Loop()
    {
        while (!_stop.IsSet)
        {
            try
            {
                EnsureHandles();

                // Wait on the event representing the state we are NOT in. Waiting on the
                // currently-signalled one would spin, because these are manual-reset.
                EventWaitHandle? target = IsSuspended ? _resume : _suspend;

                if (target is null)
                {
                    _stop.Wait(ReconcilePeriod);
                }
                else
                {
                    WaitHandle.WaitAny(new[] { target, _stop.WaitHandle },
                                       ReconcilePeriod);
                }

                if (_stop.IsSet) break;
                Reconcile();
            }
            catch (Exception ex)
            {
                _log.Report("Suspend/resume watcher error: " + ex.Message);
                _stop.Wait(ReconcilePeriod);
            }
        }
    }

    /// <summary>Moves the primary display towards whatever the compositor is currently asking for.</summary>
    public void Reconcile()
    {
        SuspensionRequest desired = ReadDesiredState();

        // The compositor is gone. If a split is still up, make sure we are not left suspended.
        if (desired == SuspensionRequest.Unknown)
        {
            if (IsSuspended && HidingStatus.IsSplitActive())
            {
                _log.Report("Compositor events disappeared while suspended — restoring the split's primary.");
                if (MoveTo(_activePrimaryDevice, "segment 1")) IsSuspended = false;
            }
            return;
        }

        bool wantSuspended = desired == SuspensionRequest.Suspended;
        if (wantSuspended == IsSuspended) return;   // already there: repeated signals are no-ops

        // Nothing to move primary between if the split is gone.
        if (!HidingStatus.IsSplitActive())
        {
            IsSuspended = false;
            return;
        }

        if (wantSuspended)
        {
            _log.Report("Lock screen / secure desktop detected — handing primary back to " +
                        $"{_suspendedPrimaryDevice}.");
            if (MoveTo(_suspendedPrimaryDevice, "the physical monitor")) IsSuspended = true;
        }
        else
        {
            _log.Report($"Secure desktop dismissed — returning primary to {_activePrimaryDevice}.");
            if (MoveTo(_activePrimaryDevice, "segment 1")) IsSuspended = false;
        }
    }

    /// <summary>
    /// Performs the primary switch under the shared display gate, so it can never interleave with
    /// a user-initiated Apply or Revert.
    /// </summary>
    private bool MoveTo(string device, string description)
    {
        if (string.IsNullOrEmpty(device)) return false;

        lock (ApplyService.DisplayOperationGate)
        {
            // Re-check inside the gate: an Apply or Revert may have changed everything while we
            // were queued behind it.
            if (!HidingStatus.IsSplitActive())
            {
                _log.Report("Split is no longer active — skipping the primary switch.");
                return false;
            }
            return ApplyService.SwitchPrimaryTo(device, description, _log);
        }
    }

    private void EnsureHandles()
    {
        _suspend ??= TryOpen(SuspendEventName);
        _resume ??= TryOpen(ResumeEventName);
    }

    /// <summary>Opens a compositor event, preferring the Global namespace and falling back to Local.</summary>
    private static EventWaitHandle? TryOpen(string name)
    {
        foreach (string prefix in new[] { @"Global\", @"Local\" })
        {
            try
            {
                if (EventWaitHandle.TryOpenExisting(prefix + name, out EventWaitHandle? handle))
                    return handle;
            }
            catch (UnauthorizedAccessException)
            {
                // Exists but this token cannot open it; try the other namespace.
            }
            catch (WaitHandleCannotBeOpenedException)
            {
                // Not created yet.
            }
        }
        return null;
    }

    private void CloseHandles()
    {
        _suspend?.Dispose();
        _resume?.Dispose();
        _suspend = null;
        _resume = null;
    }

    public void Dispose()
    {
        Stop(restorePrimary: false);
        _stop.Dispose();
    }
}
