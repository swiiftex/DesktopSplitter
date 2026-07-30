using System.Collections.ObjectModel;
using System.Threading;
using System.Threading.Tasks;
using DesktopSplitter.Interop;
using DesktopSplitter.Models;
using DesktopSplitter.Services;

namespace DesktopSplitter.ViewModels;

/// <summary>A live countdown dialog the caller can dismiss early.</summary>
public interface ICountdownHandle
{
    void CloseFromHelper();
}

/// <summary>A live guided-Settings dialog the caller can dismiss early.</summary>
public interface IGuidedHandle
{
    void CloseFromCaller();
}

/// <summary>The dialogs the hiding flows need. Implemented by the main window.</summary>
public interface IHidingUi
{
    /// <summary>Explanation dialog. Returns true to proceed.</summary>
    bool Confirm(string title, string message);

    void ShowOutcome(string title, string message, bool success);

    /// <summary>Shows the keep-or-revert countdown on <paramref name="placeOn"/>.</summary>
    ICountdownHandle ShowCountdown(int seconds, MonitorInfo? placeOn, Action onKeep, Action onRevert);

    /// <summary>
    /// Shows the generic "Keep these display settings?" countdown, centred on the primary display.
    /// </summary>
    ICountdownHandle ShowApplyCountdown(string summary, int seconds, Action onKeep, Action onRevert);

    /// <summary>
    /// Shows the guided "Remove display from desktop" instructions on <paramref name="placeOn"/>.
    /// The dialog polls the display state itself and reports through <paramref name="onFinished"/>
    /// (true when the display reached <paramref name="wantEnabled"/>, false if cancelled).
    /// </summary>
    IGuidedHandle ShowGuidedSettings(
        MonitorInfo target, bool wantEnabled, string reason, MonitorInfo? placeOn, Action<bool> onFinished);
}

public sealed class StatusRowViewModel : ObservableObject
{
    private CheckState _state;
    private string _detail = string.Empty;
    private string _label;

    public StatusRowViewModel(string label) => _label = label;

    public string Label
    {
        get => _label;
        private set => SetProperty(ref _label, value);
    }

    public CheckState State
    {
        get => _state;
        private set { if (SetProperty(ref _state, value)) OnPropertyChanged(nameof(Glyph)); }
    }

    public string Detail
    {
        get => _detail;
        private set => SetProperty(ref _detail, value);
    }

    public string Glyph => State switch
    {
        CheckState.Ok => "●",
        CheckState.No => "○",
        CheckState.Info => "·",
        _ => "—",
    };

    public void Update(StatusRow row)
    {
        Label = row.Label;
        State = row.State;
        Detail = row.Detail;
    }
}

/// <summary>
/// The "Display hiding (experimental)" panel.
///
/// Hiding uses DisplayConfig monitor specialization — the supported mechanism behind
/// Settings &gt; Advanced display &gt; "Remove display from desktop". Enable/Disable here only
/// record the INTENT; the display is actually hidden during Apply, once the split is up and the
/// physical monitor is no longer primary (Windows will not specialize a primary display).
/// </summary>
public sealed class HidingViewModel : ObservableObject
{
    private readonly Func<MonitorInfo?> _selectedMonitor;
    private readonly Action<string> _log;

    private IHidingUi? _ui;
    private bool _busy;
    private string _disabledReason = string.Empty;
    private HidingSnapshot? _snapshot;

    public HidingViewModel(Func<MonitorInfo?> selectedMonitor, Action<string> log)
    {
        _selectedMonitor = selectedMonitor;
        _log = log;

        Rows = new ObservableCollection<StatusRowViewModel>
        {
            new("Windows can hide displays"),
            new("This monitor can be hidden"),
            new("Hidden right now"),
            new("Hide during Apply"),
        };

        RefreshCommand = new RelayCommand(async () => await RefreshAsync().ConfigureAwait(false), () => !IsBusy);
        EnableCommand = new RelayCommand(async () => await SetIntentAsync(true).ConfigureAwait(false),
                                         () => CanEnable);
        DisableCommand = new RelayCommand(async () => await SetIntentAsync(false).ConfigureAwait(false),
                                          () => CanDisable);
    }

    public void AttachUi(IHidingUi ui) => _ui = ui;

    /// <summary>The main window's hiding dialogs, wrapped for <see cref="ApplyService"/>.</summary>
    public ISpecializationUi CreateSpecializationUi(Func<IReadOnlyList<MonitorInfo>> allMonitors)
        => new SpecializationUi(this, allMonitors);

    public ObservableCollection<StatusRowViewModel> Rows { get; }

    public RelayCommand RefreshCommand { get; }
    public RelayCommand EnableCommand { get; }
    public RelayCommand DisableCommand { get; }

    public bool IsBusy
    {
        get => _busy;
        private set { if (SetProperty(ref _busy, value)) RaiseStates(); }
    }

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

    /// <summary>Whether Apply should try to hide the physical monitor.</summary>
    public bool IntentOn => _snapshot?.IntentOn ?? false;

    public bool CanEnable => !IsBusy && _snapshot is { Available: true, IntentOn: false, SplitActive: false }
                             && _selectedMonitor() is not null;

    public bool CanDisable => !IsBusy && _snapshot is { IntentOn: true, SplitActive: false };

    // ------------------------------------------------------------------ status

    public async Task RefreshAsync()
    {
        MonitorInfo? selected = _selectedMonitor();
        HidingSnapshot snapshot = await Task.Run(() => HidingStatus.Query(selected)).ConfigureAwait(true);

        _snapshot = snapshot;
        IReadOnlyList<StatusRow> rows = snapshot.Rows;
        for (int i = 0; i < Rows.Count && i < rows.Count; i++) Rows[i].Update(rows[i]);

        DisabledReason = ComputeDisabledReason(snapshot, selected);
        OnPropertyChanged(nameof(IntentOn));
        RaiseStates();
    }

    private static string ComputeDisabledReason(HidingSnapshot snapshot, MonitorInfo? selected)
    {
        if (selected is null) return "Select a physical monitor first.";
        if (snapshot.SplitActive)
            return "A split is active. Press Revert first — display hiding is turned on and off during Apply.";
        if (!snapshot.Available && !snapshot.IntentOn)
            return "Windows reports this monitor cannot be removed from the desktop, so hiding would do nothing.";
        return string.Empty;
    }

    private void RaiseStates()
    {
        OnPropertyChanged(nameof(CanEnable));
        OnPropertyChanged(nameof(CanDisable));
        RefreshCommand.RaiseCanExecuteChanged();
        EnableCommand.RaiseCanExecuteChanged();
        DisableCommand.RaiseCanExecuteChanged();
    }

    // ------------------------------------------------------------------ enable / disable intent

    private async Task SetIntentAsync(bool enable)
    {
        IHidingUi? ui = _ui;
        MonitorInfo? target = _selectedMonitor();
        if (ui is null) return;
        if (enable && !CanEnable) return;
        if (!enable && !CanDisable) return;

        string title = enable ? "Enable display hiding" : "Disable display hiding";
        if (!ui.Confirm(title, BuildExplanation(target, enable))) return;

        IsBusy = true;
        try
        {
            HidingStatus.IntentWriteResult result =
                await Task.Run(() => HidingStatus.WriteIntent(enable)).ConfigureAwait(true);

            _log(result.Message);
            await RefreshAsync().ConfigureAwait(true);
            ui.ShowOutcome(title, result.Message, result.Success);
        }
        finally
        {
            IsBusy = false;
        }
    }

    private static string BuildExplanation(MonitorInfo? target, bool enable)
    {
        string name = target?.DisplayLabel ?? "the selected monitor";

        if (!enable)
        {
            return
                "Apply will stop removing the physical monitor from the desktop." +
                Environment.NewLine + Environment.NewLine +
                "If a split is running right now, press Revert to put the monitor back.";
        }

        return
            $"Target: {name} ({target?.DeviceName}){Environment.NewLine}{Environment.NewLine}" +
            "This records an intent — nothing changes yet. The next time you press Apply, " +
            "DesktopSplitter will:" + Environment.NewLine +
            "  1. Create the virtual monitors and make segment 1 the primary display." + Environment.NewLine +
            "  2. Remove the physical monitor from the desktop — the same thing as Settings > " +
            "Advanced display > \"Remove display from desktop\"." + Environment.NewLine +
            "  3. Start the compositor and wait for it to confirm it is drawing to the hidden display." +
            Environment.NewLine +
            $"  4. Ask you to confirm, with a {ApplyService.SpecializationConfirmSeconds}-second countdown." +
            Environment.NewLine + Environment.NewLine +
            "If anything looks wrong — or you cannot see the countdown at all — DO NOTHING. " +
            $"The monitor comes back automatically after {ApplyService.SpecializationConfirmSeconds} seconds " +
            "and the compositor restarts in its normal windowed mode." +
            Environment.NewLine + Environment.NewLine +
            "Revert always puts the monitor back on the desktop.";
    }

    // ------------------------------------------------------------------ keep/revert countdown

    /// <summary>
    /// Bridges <see cref="ApplyService"/> (background thread) to the countdown window (UI thread).
    /// Blocks the apply until the user answers or the countdown lapses; a lapse means REVERT, so
    /// a user who cannot see the dialog always ends up back where they started.
    /// </summary>
    private sealed class SpecializationUi : ISpecializationUi
    {
        private readonly HidingViewModel _owner;
        private readonly Func<IReadOnlyList<MonitorInfo>> _allMonitors;

        public SpecializationUi(HidingViewModel owner, Func<IReadOnlyList<MonitorInfo>> allMonitors)
        {
            _owner = owner;
            _allMonitors = allMonitors;
        }

        /// <summary>
        /// The compositor is already running and waiting to take over the display; now walk the
        /// user through Windows' own toggle. Blocks until the display actually leaves the desktop
        /// or the user cancels.
        /// </summary>
        public bool GuideManualEnable(MonitorInfo physical, SpecializationResult failure, CancellationToken ct)
            => RunGuided(physical, wantEnabled: true,
                reason: "DesktopSplitter is not allowed to do this itself — Windows refuses the request " +
                        "from an ordinary application. The compositor is already running and waiting, so " +
                        "the moment you flip the toggle your split appears on the display.",
                ct);

        /// <summary>Undo could not be done programmatically either — walk the user back out.</summary>
        public void GuideManualDisable(MonitorInfo physical, SpecializationResult failure, CancellationToken ct)
            => RunGuided(physical, wantEnabled: false,
                reason: "DesktopSplitter could not put the display back by itself, so please undo the " +
                        "toggle. This window stays here until you do.",
                ct);

        private bool RunGuided(MonitorInfo physical, bool wantEnabled, string reason, CancellationToken ct)
        {
            IHidingUi? ui = _owner._ui;
            System.Windows.Threading.Dispatcher? dispatcher = System.Windows.Application.Current?.Dispatcher;
            if (ui is null || dispatcher is null) return false;

            using var finished = new ManualResetEventSlim(false);
            int succeeded = 0;
            IGuidedHandle? handle = null;

            MonitorInfo? placeOn = PickHostMonitor(physical);
            _owner._log(wantEnabled
                ? $"Showing the Windows Settings instructions on {placeOn?.DisplayLabel ?? "the only display"}."
                : $"Showing recovery instructions on {placeOn?.DisplayLabel ?? "the only display"}.");

            dispatcher.Invoke(() =>
            {
                handle = ui.ShowGuidedSettings(physical, wantEnabled, reason, placeOn, ok =>
                {
                    Interlocked.Exchange(ref succeeded, ok ? 1 : 0);
                    finished.Set();
                });
            });

            try
            {
                finished.Wait(ct);
            }
            catch (OperationCanceledException)
            {
                dispatcher.Invoke(() => handle?.CloseFromCaller());
                return false;
            }

            return Volatile.Read(ref succeeded) == 1;
        }

        private MonitorInfo? PickHostMonitor(MonitorInfo physical)
            => CountdownPlacement.ChooseMonitor(_allMonitors(), physical.DeviceName);

        public bool ConfirmKeep(MonitorInfo physical, int timeoutSeconds, CancellationToken ct)
        {
            IHidingUi? ui = _owner._ui;
            System.Windows.Threading.Dispatcher? dispatcher =
                System.Windows.Application.Current?.Dispatcher;
            if (ui is null || dispatcher is null) return false;

            using var answered = new ManualResetEventSlim(false);
            int keep = 0;
            ICountdownHandle? handle = null;

            MonitorInfo? placeOn = PickHostMonitor(physical);
            _owner._log(placeOn is null || SameDevice(placeOn.DeviceName, physical.DeviceName)
                ? "Countdown will appear on the only available display."
                : $"Countdown will appear on {placeOn.DisplayLabel} ({placeOn.DeviceName}), " +
                  "away from the hidden monitor.");

            dispatcher.Invoke(() =>
            {
                handle = ui.ShowCountdown(timeoutSeconds, placeOn,
                    onKeep: () => { Interlocked.Exchange(ref keep, 1); answered.Set(); },
                    onRevert: () => { Interlocked.Exchange(ref keep, 0); answered.Set(); });
            });

            // The window's own timer only animates; THIS wait is the auto-revert authority.
            bool gotAnswer = answered.Wait(TimeSpan.FromSeconds(timeoutSeconds + 1), ct);

            dispatcher.Invoke(() => handle?.CloseFromHelper());

            if (!gotAnswer)
            {
                _owner._log($"No answer within {timeoutSeconds}s — reverting automatically.");
                return false;
            }
            return Volatile.Read(ref keep) == 1;
        }

        private static bool SameDevice(string a, string b)
            => string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
    }
}
