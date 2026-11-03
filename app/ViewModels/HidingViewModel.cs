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
            new("Windows 可隐藏显示器"),
            new("此显示器可被隐藏"),
            new("当前已隐藏"),
            new("应用时隐藏"),
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
        if (selected is null) return "请先选择一台物理显示器。";
        if (snapshot.SplitActive)
            return "当前有正在运行的分屏。请先点「撤销」——显示器隐藏只能在应用分屏时开启或关闭。";
        if (!snapshot.Available && !snapshot.IntentOn)
            return "Windows 报告此显示器无法从桌面移除，隐藏不会有任何效果。";
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

        string title = enable ? "启用显示器隐藏" : "禁用显示器隐藏";
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
        string name = target?.DisplayLabel ?? "所选显示器";

        if (!enable)
        {
            return
                "应用分屏时将不再把物理显示器从桌面移除。" +
                Environment.NewLine + Environment.NewLine +
                "如果当前有分屏正在运行，请点「撤销」把显示器恢复到桌面。";
        }

        return
            $"目标：{name}（{target?.DeviceName}）{Environment.NewLine}{Environment.NewLine}" +
            "这只是记录一个意向——现在还不会有任何变化。下次你点「应用」时，" +
            "DesktopSplitter 将：" + Environment.NewLine +
            "  1. 创建虚拟显示器，并把分区 1 设为主显示器。" + Environment.NewLine +
            "  2. 把物理显示器从桌面移除——与「设置 > " +
            "高级显示 > 将显示器从桌面移除」相同。" + Environment.NewLine +
            "  3. 启动合成器，并等待它确认正在向被隐藏的显示器输出画面。" +
            Environment.NewLine +
            $"  4. 请你确认，并给出 {ApplyService.SpecializationConfirmSeconds} 秒倒计时。" +
            Environment.NewLine + Environment.NewLine +
            "如果画面有任何异常——或者你根本看不到倒计时——什么都不要做。" +
            $"{ApplyService.SpecializationConfirmSeconds} 秒后显示器会自动恢复，" +
            "合成器会以普通窗口模式重启。" +
            Environment.NewLine + Environment.NewLine +
            "点「撤销」随时可以把显示器恢复到桌面。";
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
                reason: "DesktopSplitter 无权自行完成此操作——Windows 会拒绝普通应用程序的" +
                        "这类请求。合成器已经在运行并等待，你一拨动开关，分屏就会出现" +
                        "在显示器上。",
                ct);

        /// <summary>Undo could not be done programmatically either — walk the user back out.</summary>
        public void GuideManualDisable(MonitorInfo physical, SpecializationResult failure, CancellationToken ct)
            => RunGuided(physical, wantEnabled: false,
                reason: "DesktopSplitter 无法自行把显示器恢复，请你手动撤销该开关。" +
                        "在你完成之前，这个窗口会一直停在这里。",
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
                ? $"正在 {placeOn?.DisplayLabel ?? "唯一的显示器"} 上显示 Windows 设置操作说明。"
                : $"正在 {placeOn?.DisplayLabel ?? "唯一的显示器"} 上显示恢复操作说明。");

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
                ? "倒计时将显示在唯一可用的显示器上。"
                : $"倒计时将显示在 {placeOn.DisplayLabel}（{placeOn.DeviceName}）上，" +
                  "避开被隐藏的显示器。");

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
                _owner._log($"{timeoutSeconds} 秒内未收到答复——自动撤销。");
                return false;
            }
            return Volatile.Read(ref keep) == 1;
        }

        private static bool SameDevice(string a, string b)
            => string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
    }
}
