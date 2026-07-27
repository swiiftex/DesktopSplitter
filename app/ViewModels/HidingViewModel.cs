using System.Collections.ObjectModel;
using System.Threading;
using System.Threading.Tasks;
using DesktopSplitter.Models;
using DesktopSplitter.Services;

namespace DesktopSplitter.ViewModels;

/// <summary>A live countdown dialog the view-model can dismiss when the helper finishes.</summary>
public interface ICountdownHandle
{
    void CloseFromHelper();
}

/// <summary>The dialogs the hiding flows need. Implemented by the main window.</summary>
public interface IHidingUi
{
    /// <summary>Explanation dialog. Returns true to proceed.</summary>
    bool Confirm(string title, string message);

    void ShowOutcome(string title, string message, bool success);

    /// <summary>Shows the keep-or-revert countdown on <paramref name="placeOn"/>.</summary>
    ICountdownHandle ShowCountdown(int seconds, MonitorInfo? placeOn, Action onKeep, Action onRevert);
}

public sealed class StatusRowViewModel : ObservableObject
{
    private CheckState _state;
    private string _detail = string.Empty;

    public StatusRowViewModel(string label) => Label = label;

    public string Label { get; }

    public CheckState State
    {
        get => _state;
        set { if (SetProperty(ref _state, value)) OnPropertyChanged(nameof(Glyph)); }
    }

    public string Detail
    {
        get => _detail;
        set => SetProperty(ref _detail, value);
    }

    public string Glyph => State switch
    {
        CheckState.Ok => "●",     // filled dot
        CheckState.No => "○",     // hollow dot
        _ => "—",                 // em dash
    };

    public void Update(StatusRow row)
    {
        State = row.State;
        Detail = row.Detail;
    }
}

/// <summary>
/// The "Display hiding (experimental)" panel: status probes plus the guarded enable/disable
/// flows that wrap edidoverride.exe.
/// </summary>
public sealed class HidingViewModel : ObservableObject
{
    /// <summary>Matches the helper's own default; it remains the authority on the real timer.</summary>
    public const int TimeoutSeconds = 10;

    private readonly Func<MonitorInfo?> _selectedMonitor;
    private readonly Func<IReadOnlyList<MonitorInfo>> _allMonitors;
    private readonly Action<string> _log;
    private readonly IProgress<string> _progress;
    private readonly SynchronizationContext _uiContext;

    private IHidingUi? _ui;
    private bool _busy;
    private string _disabledReason = string.Empty;
    private HidingSnapshot? _snapshot;

    public HidingViewModel(
        Func<MonitorInfo?> selectedMonitor,
        Func<IReadOnlyList<MonitorInfo>> allMonitors,
        Action<string> log)
    {
        _selectedMonitor = selectedMonitor;
        _allMonitors = allMonitors;
        _log = log;
        _progress = new Progress<string>(log);
        _uiContext = SynchronizationContext.Current ?? new SynchronizationContext();

        Rows = new ObservableCollection<StatusRowViewModel>
        {
            new("Windows edition"),
            new("Monitor is hideable"),
            new("DesktopSplitter override"),
            new("HidePhysicalDisplay flag"),
        };

        RefreshCommand = new RelayCommand(async () => await RefreshAsync().ConfigureAwait(false), () => !IsBusy);
        EnableCommand = new RelayCommand(async () => await EnableAsync(dryRun: false).ConfigureAwait(false),
                                         () => CanEnable);
        DisableCommand = new RelayCommand(async () => await DisableAsync().ConfigureAwait(false), () => CanDisable);
        TestRunCommand = new RelayCommand(async () => await EnableAsync(dryRun: true).ConfigureAwait(false),
                                          () => CanTestRun);
    }

    /// <summary>Supplied by the view once it is constructed.</summary>
    public void AttachUi(IHidingUi ui) => _ui = ui;

    public ObservableCollection<StatusRowViewModel> Rows { get; }

    public RelayCommand RefreshCommand { get; }
    public RelayCommand EnableCommand { get; }
    public RelayCommand DisableCommand { get; }
    public RelayCommand TestRunCommand { get; }

    public bool IsBusy
    {
        get => _busy;
        private set
        {
            if (!SetProperty(ref _busy, value)) return;
            RaiseStates();
        }
    }

    /// <summary>Why the action buttons are greyed out, or "" when they are usable.</summary>
    public string DisabledReason
    {
        get => _disabledReason;
        private set
        {
            if (!SetProperty(ref _disabledReason, value)) return;
            OnPropertyChanged(nameof(HasDisabledReason));
        }
    }

    public bool HasDisabledReason => !string.IsNullOrEmpty(_disabledReason);

    public bool CanEnable => !IsBusy && _snapshot is { OsSupported: true, SplitActive: false, HelperFound: true }
                             && !_snapshot.OverrideIsOurs && _selectedMonitor() is not null;

    public bool CanDisable => !IsBusy && _snapshot is { HelperFound: true, SplitActive: false }
                              && (_snapshot.OverrideIsOurs || _snapshot.FlagOn) && _selectedMonitor() is not null;

    /// <summary>The dry run only needs the helper — it changes nothing and needs no elevation.</summary>
    public bool CanTestRun => !IsBusy && _snapshot is { HelperFound: true } && _selectedMonitor() is not null;

    // ------------------------------------------------------------------ status

    public async Task RefreshAsync()
    {
        MonitorInfo? selected = _selectedMonitor();
        HidingSnapshot snapshot = await Task.Run(() => HidingStatus.QueryAsync(selected)).ConfigureAwait(true);

        _snapshot = snapshot;
        IReadOnlyList<StatusRow> rows = snapshot.Rows;
        for (int i = 0; i < Rows.Count && i < rows.Count; i++) Rows[i].Update(rows[i]);

        DisabledReason = ComputeDisabledReason(snapshot, selected);
        RaiseStates();
    }

    private static string ComputeDisabledReason(HidingSnapshot snapshot, MonitorInfo? selected)
    {
        if (selected is null) return "Select a physical monitor first.";
        if (!snapshot.OsSupported)
            return "This Windows edition cannot drive a specialized display, so hiding would have no effect.";
        if (!snapshot.HelperFound)
            return $"edidoverride.exe not found — expected at {snapshot.HelperPath}.";
        if (snapshot.SplitActive)
            return "A split is active. Press Revert first — the monitor cannot be re-enumerated while it is in use.";
        return string.Empty;
    }

    private void RaiseStates()
    {
        OnPropertyChanged(nameof(CanEnable));
        OnPropertyChanged(nameof(CanDisable));
        OnPropertyChanged(nameof(CanTestRun));
        RefreshCommand.RaiseCanExecuteChanged();
        EnableCommand.RaiseCanExecuteChanged();
        DisableCommand.RaiseCanExecuteChanged();
        TestRunCommand.RaiseCanExecuteChanged();
    }

    // ------------------------------------------------------------------ enable / test run

    private async Task EnableAsync(bool dryRun)
    {
        MonitorInfo? target = _selectedMonitor();
        IHidingUi? ui = _ui;
        if (target is null || ui is null) return;
        if (!dryRun && !CanEnable) return;
        if (dryRun && !CanTestRun) return;

        string title = dryRun ? "Test run (no changes)" : "Enable display hiding";
        if (!ui.Confirm(title, BuildExplanation(target, dryRun))) return;

        IsBusy = true;
        _log($"--- {(dryRun ? "Display hiding TEST RUN" : "Enabling display hiding")} on {target.DeviceName} ---");

        try
        {
            GuardResult result = await RunGuardedAsync(target, dryRun).ConfigureAwait(true);
            await RefreshAsync().ConfigureAwait(true);

            ui.ShowOutcome(title, DescribeOutcome(result, dryRun), result.Succeeded);

            if (result.Succeeded && !dryRun)
            {
                ui.ShowOutcome("Display hiding is armed",
                    "Hiding is armed. Apply a split now to use it — the compositor picks the specialized " +
                    "presenter automatically from the HidePhysicalDisplay flag.", true);
            }
        }
        finally
        {
            IsBusy = false;
        }
    }

    /// <summary>
    /// Runs the guarded sequence and drives the countdown. The countdown is shown when the
    /// helper reports Armed, and torn down as soon as the helper exits — however it exits.
    /// </summary>
    private async Task<GuardResult> RunGuardedAsync(MonitorInfo target, bool dryRun)
    {
        var session = new EdidGuardSession();
        ICountdownHandle? countdown = null;
        IHidingUi ui = _ui!;

        MonitorInfo? placeOn = CountdownPlacement.ChooseMonitor(_allMonitors(), target.DeviceName);
        if (placeOn is not null && !string.Equals(placeOn.DeviceName, target.DeviceName, StringComparison.OrdinalIgnoreCase))
            _log($"Countdown will appear on {placeOn.DisplayLabel} ({placeOn.DeviceName}), away from the target.");
        else
            _log("Only one display available — the countdown will appear on it.");

        session.Armed += seconds => _uiContext.Post(_ =>
        {
            countdown = ui.ShowCountdown(
                seconds, placeOn,
                onKeep: () => session.RequestKeep(),
                onRevert: () => session.RequestRevert());
        }, null);

        try
        {
            return await session
                .ApplyAsync(target.DeviceName, dryRun, TimeoutSeconds, _progress, CancellationToken.None)
                .ConfigureAwait(true);
        }
        finally
        {
            _uiContext.Post(_ => countdown?.CloseFromHelper(), null);
        }
    }

    // ------------------------------------------------------------------ disable

    private async Task DisableAsync()
    {
        MonitorInfo? target = _selectedMonitor();
        IHidingUi? ui = _ui;
        if (target is null || ui is null || !CanDisable) return;

        string message =
            $"This removes the DesktopSplitter EDID override from {target.DisplayLabel} ({target.DeviceName}) " +
            "and clears the HidePhysicalDisplay flag, returning it to an ordinary desktop monitor." +
            Environment.NewLine + Environment.NewLine +
            "The monitor will be restarted, so the screen may blink." + Environment.NewLine + Environment.NewLine +
            "Windows will ask for administrator rights.";

        if (!ui.Confirm("Disable display hiding", message)) return;

        IsBusy = true;
        _log($"--- Disabling display hiding on {target.DeviceName} ---");
        try
        {
            var session = new EdidGuardSession();
            GuardResult result = await session
                .RevertAsync(target.DeviceName, dryRun: false, _progress, CancellationToken.None)
                .ConfigureAwait(true);

            await RefreshAsync().ConfigureAwait(true);
            ui.ShowOutcome("Disable display hiding", result.Message, result.Succeeded);
        }
        finally
        {
            IsBusy = false;
        }
    }

    // ------------------------------------------------------------------ text

    private static string BuildExplanation(MonitorInfo target, bool dryRun)
    {
        string what = dryRun
            ? "This is a REHEARSAL. edidoverride.exe walks the entire sequence — including this " +
              "countdown and every exit code — but changes absolutely nothing, and does not need " +
              "administrator rights."
            : "DesktopSplitter will patch the EDID that Windows reads for this monitor so Windows " +
              "treats it as a specialized display and removes it from the desktop.";

        return
            $"Target: {target.DisplayLabel} ({target.DeviceName}){Environment.NewLine}{Environment.NewLine}" +
            what + Environment.NewLine + Environment.NewLine +
            "What happens next:" + Environment.NewLine +
            "  1. The EDID override is written and the monitor is restarted." + Environment.NewLine +
            "  2. The screen may blink or go briefly dark." + Environment.NewLine +
            $"  3. A countdown appears and you have {TimeoutSeconds} seconds to confirm." +
            Environment.NewLine + Environment.NewLine +
            "If anything looks wrong — or you cannot see the countdown at all — DO NOTHING. " +
            $"The change reverts itself automatically after {TimeoutSeconds} seconds." +
            Environment.NewLine + Environment.NewLine +
            (dryRun ? "Nothing will be modified." : "Windows will ask for administrator rights.");
    }

    private static string DescribeOutcome(GuardResult result, bool dryRun) => result.Outcome switch
    {
        GuardOutcome.Kept => dryRun
            ? "Rehearsal complete: the Keep path worked end to end. Nothing was changed."
            : result.Message,
        GuardOutcome.RevertedByUser => dryRun
            ? "Rehearsal complete: the Revert path worked end to end. Nothing was changed."
            : result.Message,
        GuardOutcome.AutoReverted => dryRun
            ? "Rehearsal complete: the timeout path worked end to end. Nothing was changed."
            : result.Message,
        _ => result.Message,
    };
}
