using System.Collections.ObjectModel;
using System.Threading;
using System.Threading.Tasks;
using DesktopSplitter.Interop;
using DesktopSplitter.Models;
using DesktopSplitter.Services;

namespace DesktopSplitter.ViewModels;

public sealed class MainViewModel : ObservableObject
{
    /// <summary>The editor canvas is fitted into this box, preserving the monitor's aspect.</summary>
    private const double MaxCanvasWidth = 660;
    private const double MaxCanvasHeight = 380;

    private readonly IProgress<string> _progress;
    private readonly object _statusLock = new();

    private MonitorInfo? _selectedMonitor;
    private LayoutDefinition? _selectedLayout;
    private string _statusText = string.Empty;
    private string _driverStatus = string.Empty;
    private string _refreshText = "—";
    private string _currentModeText = "—";
    private string _compositorStatus = string.Empty;
    private string _sumNote = string.Empty;
    private double _canvasWidth = MaxCanvasWidth;
    private double _canvasHeight = 200;
    private bool _isBusy;
    private bool _suppressSegmentRebuild;
    private IApplyConfirmer? _applyConfirmer;
    private SplitSuspensionWatcher? _suspensionWatcher;

    /// <summary>Set while the VM itself is writing segment sizes, to stop feedback loops.</summary>
    private bool _editorSuspended;

    // Splitter drag state.
    private SplitterVisual? _dragSplitter;
    private int[] _dragTracks = Array.Empty<int>();
    private double _dragGrabOffset;

    public MainViewModel()
    {
        // Constructed on the UI thread: Progress<T> captures the dispatcher context.
        _progress = new Progress<string>(AppendStatus);

        Layouts = new ObservableCollection<LayoutDefinition>(LayoutDefinition.All);
        Monitors = new ObservableCollection<MonitorInfo>();
        Segments = new ObservableCollection<SegmentViewModel>();
        Zones = new ObservableCollection<ZoneVisual>();
        Splitters = new ObservableCollection<SplitterVisual>();

        RefreshMonitorsCommand = new RelayCommand(() => LoadMonitors(announce: true), () => !IsBusy);
        ResetSegmentsCommand = new RelayCommand(() => RebuildSegments(resetSizes: true),
                                                () => !IsBusy && SelectedLayout is not null);
        ApplyCommand = new RelayCommand(async () => await ApplyAsync().ConfigureAwait(false), () => CanApply);
        RevertCommand = new RelayCommand(async () => await RevertAsync().ConfigureAwait(false), () => !IsBusy);
        MinimizeToTrayCommand = new RelayCommand(() => MinimizeToTrayRequested?.Invoke(this, EventArgs.Empty));

        _selectedLayout = Layouts[0];

        Hiding = new HidingViewModel(() => SelectedMonitor, AppendStatus);

        LoadMonitors(announce: false);
        RestoreSettings();
        RefreshDriverStatus();
        RefreshCompositorStatus();

        AppendStatus("Ready.");

        // If a previous session died while the physical monitor was hidden, put it back.
        _ = RecoverThenRefreshAsync();
    }

    // ------------------------------------------------------------------ bound state

    /// <summary>The "Display hiding (experimental)" panel.</summary>
    public HidingViewModel Hiding { get; }

    /// <summary>Informational assembly version, produced by the shared versioning build step.</summary>
    public static string VersionText { get; } = ReadVersion();

    private static string ReadVersion()
    {
        try
        {
            System.Reflection.Assembly assembly = System.Reflection.Assembly.GetExecutingAssembly();
            string? informational = System.Reflection.CustomAttributeExtensions
                .GetCustomAttribute<System.Reflection.AssemblyInformationalVersionAttribute>(assembly)
                ?.InformationalVersion;

            // Strip any "+<commit>" source-revision suffix the SDK appends.
            if (!string.IsNullOrWhiteSpace(informational))
            {
                int plus = informational.IndexOf('+');
                return "v" + (plus > 0 ? informational[..plus] : informational);
            }
            return "v" + (assembly.GetName().Version?.ToString() ?? "1.0.0");
        }
        catch
        {
            return string.Empty;
        }
    }

    /// <summary>Supplied by the view once it exists; enables the confirmation countdown.</summary>
    public void AttachConfirmer(IApplyConfirmer confirmer) => _applyConfirmer = confirmer;

    // ------------------------------------------------------------------ suspend / resume

    /// <summary>
    /// Starts handing primary back to the physical monitor whenever the compositor reports a
    /// secure desktop, lock screen, or the manual Ctrl+Alt+Shift+S toggle.
    /// </summary>
    private void StartSuspensionWatcher()
    {
        AppSettings? settings = SettingsStore.Load();
        if (settings is null) return;

        string activePrimary = settings.AppliedPrimaryDevice;
        string physicalPrimary = string.IsNullOrEmpty(settings.PreviousPrimaryDevice)
            ? settings.PhysicalDevice
            : settings.PreviousPrimaryDevice;

        _suspensionWatcher ??= new SplitSuspensionWatcher(_progress);
        _suspensionWatcher.Start(activePrimary, physicalPrimary);
    }

    /// <summary>
    /// Stops the watcher. A Revert restores primary itself, so it must not also be restored here —
    /// that would be pointless extra display churn.
    /// </summary>
    private void StopSuspensionWatcher(bool restorePrimary)
    {
        _suspensionWatcher?.Stop(restorePrimary);
    }

    // ------------------------------------------------------------------ preferences

    private bool _startWithWindows;
    private bool _applyOnStartup;

    /// <summary>Backed by the HKCU Run key, which is read live so external changes show up.</summary>
    public bool StartWithWindows
    {
        get => _startWithWindows;
        set
        {
            if (!SetProperty(ref _startWithWindows, value)) return;
            StartupRegistration.Set(value, _progress);
            PersistPreferences();
            // Re-read: if the write failed, the checkbox must not lie.
            bool actual = StartupRegistration.IsEnabled();
            if (actual != value) SetProperty(ref _startWithWindows, actual, nameof(StartWithWindows));
        }
    }

    /// <summary>Re-apply the saved configuration automatically at startup.</summary>
    public bool ApplyOnStartup
    {
        get => _applyOnStartup;
        set
        {
            if (!SetProperty(ref _applyOnStartup, value)) return;
            PersistPreferences();
        }
    }

    /// <summary>Reads preference state from the registry and settings file.</summary>
    public void LoadPreferences()
    {
        StartupRegistration.RefreshIfStale(_progress);
        _startWithWindows = StartupRegistration.IsEnabled();
        _applyOnStartup = SettingsStore.Load()?.ApplyOnStartup ?? false;
        OnPropertyChanged(nameof(StartWithWindows));
        OnPropertyChanged(nameof(ApplyOnStartup));
    }

    private void PersistPreferences()
    {
        try
        {
            AppSettings settings = SettingsStore.Load() ?? new AppSettings();
            settings.StartWithWindows = _startWithWindows;
            settings.ApplyOnStartup = _applyOnStartup;
            SettingsStore.Save(settings);
        }
        catch (Exception ex)
        {
            AppendStatus("Could not save preferences: " + ex.Message);
        }
    }

    // ------------------------------------------------------------------ startup / shutdown

    /// <summary>
    /// Re-applies the saved configuration if it was live when we last exited, or if the user
    /// asked for auto-apply. Never prompts — this is not a user-initiated change.
    /// </summary>
    public async Task AutoApplyOnStartupAsync()
    {
        AppSettings? settings = SettingsStore.Load();
        bool splitActive = await Task.Run(HidingStatus.IsSplitActive).ConfigureAwait(true);

        if (!ApplyService.ShouldAutoApplyOnStartup(settings, splitActive)) return;
        if (!RestoreSettings(settings))
        {
            AppendStatus("Skipping auto-apply: the saved monitor is not connected.");
            return;
        }

        AppendStatus("--- Re-applying the saved configuration (no confirmation at startup) ---");
        await ApplyAsync(promptToConfirm: false).ConfigureAwait(true);
    }

    /// <summary>
    /// Tears the split down on exit so the desktop is never left split with nothing driving it.
    /// Best effort and time-bounded: it must not hang logoff or shutdown.
    /// </summary>
    public void RevertOnExit()
    {
        try
        {
            StopSuspensionWatcher(restorePrimary: false);

            if (!HidingStatus.IsSplitActive())
            {
                ApplyService.MarkSplitInactive(deliberate: false);
                return;
            }

            var log = new Progress<string>(_ => { });
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(20));
            ApplyService.Revert(log, cts.Token);

            // Exiting is not a deliberate revert: the configuration should come back next launch.
            AppSettings? settings = SettingsStore.Load();
            if (settings is not null)
            {
                settings.SplitActiveOnExit = true;
                SettingsStore.Save(settings);
            }
        }
        catch
        {
            // Shutdown must proceed regardless.
        }
    }

    public ObservableCollection<MonitorInfo> Monitors { get; }
    public ObservableCollection<LayoutDefinition> Layouts { get; }
    public ObservableCollection<SegmentViewModel> Segments { get; }

    /// <summary>Numbered zones drawn on the editor canvas.</summary>
    public ObservableCollection<ZoneVisual> Zones { get; }

    /// <summary>Draggable borders between adjacent zones.</summary>
    public ObservableCollection<SplitterVisual> Splitters { get; }

    public MonitorInfo? SelectedMonitor
    {
        get => _selectedMonitor;
        set
        {
            if (!SetProperty(ref _selectedMonitor, value)) return;
            UpdateModeTexts();
            RebuildSegments(resetSizes: true);
            RaiseCommandStates();
            _ = Hiding.RefreshAsync();
        }
    }

    public LayoutDefinition? SelectedLayout
    {
        get => _selectedLayout;
        set
        {
            if (!SetProperty(ref _selectedLayout, value)) return;
            RebuildSegments(resetSizes: true);
            RaiseCommandStates();
        }
    }

    /// <summary>Read-only: every segment runs at the physical monitor's max refresh.</summary>
    public string RefreshText
    {
        get => _refreshText;
        private set => SetProperty(ref _refreshText, value);
    }

    /// <summary>The selected monitor's mode right now, e.g. "5120x1440 @ 240 Hz".</summary>
    public string CurrentModeText
    {
        get => _currentModeText;
        private set => SetProperty(ref _currentModeText, value);
    }

    public string DriverStatus
    {
        get => _driverStatus;
        private set => SetProperty(ref _driverStatus, value);
    }

    public string CompositorStatus
    {
        get => _compositorStatus;
        private set => SetProperty(ref _compositorStatus, value);
    }

    /// <summary>
    /// Subtle informational note (not an error) shown when the segment sizes do not add up
    /// to the monitor dimension — the compositor scales each capture into its physRect.
    /// </summary>
    public string SumNote
    {
        get => _sumNote;
        private set
        {
            if (!SetProperty(ref _sumNote, value)) return;
            OnPropertyChanged(nameof(HasSumNote));
        }
    }

    public bool HasSumNote => !string.IsNullOrEmpty(_sumNote);

    public double CanvasWidth
    {
        get => _canvasWidth;
        private set => SetProperty(ref _canvasWidth, value);
    }

    public double CanvasHeight
    {
        get => _canvasHeight;
        private set => SetProperty(ref _canvasHeight, value);
    }

    public string StatusText
    {
        get => _statusText;
        private set => SetProperty(ref _statusText, value);
    }

    public bool IsBusy
    {
        get => _isBusy;
        private set
        {
            if (!SetProperty(ref _isBusy, value)) return;
            OnPropertyChanged(nameof(IsIdle));
            RaiseCommandStates();
        }
    }

    public bool IsIdle => !_isBusy;

    public bool CanApply =>
        !IsBusy &&
        SelectedMonitor is not null &&
        SelectedLayout is not null &&
        Segments.Count == SelectedLayout.SegmentCount &&
        Segments.All(s => s.IsValid);

    // ------------------------------------------------------------------ commands

    public RelayCommand RefreshMonitorsCommand { get; }
    public RelayCommand ResetSegmentsCommand { get; }
    public RelayCommand ApplyCommand { get; }
    public RelayCommand RevertCommand { get; }
    public RelayCommand MinimizeToTrayCommand { get; }

    public event EventHandler? MinimizeToTrayRequested;

    /// <summary>
    /// Crash recovery: a previous session may have died with the physical monitor still removed
    /// from the desktop. Awaiting (rather than TaskScheduler.FromCurrentSynchronizationContext)
    /// keeps this working when the view-model is constructed without a synchronisation context.
    /// </summary>
    private async Task RecoverThenRefreshAsync()
    {
        try
        {
            await Task.Run(() =>
            {
                SpecializationHashStore.SeedKnownHashes(Monitors.ToList(), _progress);
                ApplyService.RecoverStaleSpecialization(_progress);
            }).ConfigureAwait(true);
            await Hiding.RefreshAsync().ConfigureAwait(true);
        }
        catch (Exception ex)
        {
            AppendStatus("Display-hiding recovery check failed: " + ex.Message);
        }
    }

    // ------------------------------------------------------------------ monitor list

    /// <summary>Called from the WM_DISPLAYCHANGE hook; keeps the user's edits.</summary>
    public void OnDisplayChanged()
    {
        LoadMonitors(announce: false);
        UpdateModeTexts();
        AppendStatus("Display configuration changed — monitor list refreshed.");
    }

    public void LoadMonitors(bool announce)
    {
        string? previous = SelectedMonitor?.DeviceName;
        try
        {
            IReadOnlyList<MonitorInfo> physical = DisplayConfig.EnumeratePhysical();

            Monitors.Clear();
            foreach (MonitorInfo m in physical) Monitors.Add(m);

            _suppressSegmentRebuild = true;
            try
            {
                _selectedMonitor = Monitors.FirstOrDefault(
                                       m => string.Equals(m.DeviceName, previous, StringComparison.OrdinalIgnoreCase))
                                   ?? Monitors.FirstOrDefault(m => m.IsPrimary)
                                   ?? Monitors.FirstOrDefault();
            }
            finally { _suppressSegmentRebuild = false; }

            OnPropertyChanged(nameof(SelectedMonitor));
            UpdateModeTexts();

            // Preserve the user's edited sizes across a plain rescan of the same monitor.
            bool sameSelection = previous is not null &&
                                 string.Equals(_selectedMonitor?.DeviceName, previous, StringComparison.OrdinalIgnoreCase);
            bool countMatches = SelectedLayout is not null && Segments.Count == SelectedLayout.SegmentCount;
            RebuildSegments(resetSizes: !(sameSelection && countMatches));

            if (announce)
                AppendStatus($"Found {Monitors.Count} physical monitor(s).");
        }
        catch (Exception ex)
        {
            AppendStatus("Monitor enumeration failed: " + ex.Message);
        }
        RaiseCommandStates();
    }

    private void UpdateModeTexts()
    {
        MonitorInfo? m = SelectedMonitor;
        if (m is null)
        {
            RefreshText = "—";
            CurrentModeText = "—";
            return;
        }

        CurrentModeText = m.CurrentModeText;
        RefreshText = $"{m.MaxRefreshHz} Hz  ({m.MaxRefreshMillihertz} mHz)  — native {m.MaxWidth}x{m.MaxHeight}";
    }

    // ------------------------------------------------------------------ segments + editor

    /// <summary>
    /// Rebuilds the segment editors for the current layout. When <paramref name="resetSizes"/>
    /// is true the sizes go back to the layout's equal subdivision of the monitor; otherwise
    /// existing values are kept (used for rescans / display-change events).
    /// </summary>
    private void RebuildSegments(bool resetSizes)
    {
        if (_suppressSegmentRebuild) return;

        LayoutDefinition? layout = SelectedLayout;
        if (layout is null) return;

        MonitorInfo? monitor = SelectedMonitor;
        int monW = monitor?.CurrentWidth ?? 1920;
        int monH = monitor?.CurrentHeight ?? 1080;

        var previous = Segments.Select(s => (s.Width, s.Height)).ToList();
        bool reuse = !resetSizes && previous.Count == layout.SegmentCount;

        // Per-layout defaults: most layouts subdivide equally, the 16:9-centre preset computes
        // a true 16:9 middle segment instead.
        IReadOnlyList<(int Width, int Height)> defaults = layout.ComputeDefaultSegmentSizes(monW, monH);

        foreach (SegmentViewModel old in Segments) old.Changed -= OnSegmentChanged;
        Segments.Clear();

        for (int i = 0; i < layout.SegmentCount; i++)
        {
            (int w, int h) = reuse ? previous[i] : defaults[i];
            var vm = new SegmentViewModel(i, w, h);
            vm.Changed += OnSegmentChanged;
            Segments.Add(vm);
        }

        RebuildEditor();
        RaiseCommandStates();
    }

    private void OnSegmentChanged(object? sender, EventArgs e)
    {
        // Manual entry: only the edited segment changed. Redraw proportionally, do not
        // auto-fill the neighbours.
        if (!_editorSuspended) RebuildEditor();
        RaiseCommandStates();
    }

    /// <summary>Recomputes canvas size, zone rects, splitter bars and the sum note.</summary>
    private void RebuildEditor()
    {
        LayoutDefinition? layout = SelectedLayout;
        MonitorInfo? monitor = SelectedMonitor;

        if (layout is null || monitor is null || Segments.Count != layout.SegmentCount)
        {
            Zones.Clear();
            Splitters.Clear();
            SumNote = string.Empty;
            return;
        }

        int monW = Math.Max(1, monitor.CurrentWidth);
        int monH = Math.Max(1, monitor.CurrentHeight);

        double scale = Math.Min(MaxCanvasWidth / monW, MaxCanvasHeight / monH);
        CanvasWidth = Math.Max(120, Math.Round(monW * scale));
        CanvasHeight = Math.Max(80, Math.Round(monH * scale));

        var sizes = Segments.Select(s => (s.Width, s.Height)).ToList();
        IReadOnlyList<ZoneRect> rects =
            LayoutGeometry.ComputeZoneRects(layout, CanvasWidth, CanvasHeight, sizes);

        // Update in place when the shape is unchanged so dragging does not re-create visuals.
        if (Zones.Count != layout.SegmentCount)
        {
            Zones.Clear();
            for (int i = 0; i < layout.SegmentCount; i++) Zones.Add(new ZoneVisual(i));
        }
        for (int i = 0; i < layout.SegmentCount; i++)
        {
            ZoneVisual z = Zones[i];
            z.X = rects[i].X;
            z.Y = rects[i].Y;
            z.W = rects[i].W;
            z.H = rects[i].H;
            z.SizeText = $"{sizes[i].Width}x{sizes[i].Height}";
            z.AspectNote = ZoneVisual.DescribeAspect(sizes[i].Width, sizes[i].Height);
        }

        RebuildSplitters(layout, rects);
        UpdateSumNote(layout, sizes, monW, monH);
    }

    private void RebuildSplitters(LayoutDefinition layout, IReadOnlyList<ZoneRect> rects)
    {
        int expected = layout.VerticalSplitLines.Count + layout.HorizontalSplitLines.Count;
        if (Splitters.Count != expected || !SplittersMatch(layout))
        {
            Splitters.Clear();
            int id = 0;
            foreach (int line in layout.VerticalSplitLines) Splitters.Add(new SplitterVisual(id++, true, line));
            foreach (int line in layout.HorizontalSplitLines) Splitters.Add(new SplitterVisual(id++, false, line));
        }

        foreach (SplitterVisual s in Splitters)
        {
            // Affected segments are those touching the line from either side; the bar spans
            // exactly their union on the cross axis (so e.g. the big-left layout's horizontal
            // splitter only covers the right-hand column).
            int[] affected = s.IsVertical
                ? layout.SegmentsLeftOf(s.Line).Concat(layout.SegmentsRightOf(s.Line)).ToArray()
                : layout.SegmentsAbove(s.Line).Concat(layout.SegmentsBelow(s.Line)).ToArray();
            if (affected.Length == 0) continue;

            if (s.IsVertical)
            {
                double x = layout.SegmentsLeftOf(s.Line)
                    .Select(i => rects[i].X + rects[i].W)
                    .DefaultIfEmpty(0)
                    .Max();
                double top = affected.Select(i => rects[i].Y).Min();
                double bottom = affected.Select(i => rects[i].Y + rects[i].H).Max();

                s.X = x - SplitterVisual.Thickness / 2;
                s.Y = top;
                s.Length = Math.Max(1, bottom - top);
            }
            else
            {
                double y = layout.SegmentsAbove(s.Line)
                    .Select(i => rects[i].Y + rects[i].H)
                    .DefaultIfEmpty(0)
                    .Max();
                double left = affected.Select(i => rects[i].X).Min();
                double right = affected.Select(i => rects[i].X + rects[i].W).Max();

                s.X = left;
                s.Y = y - SplitterVisual.Thickness / 2;
                s.Length = Math.Max(1, right - left);
            }
        }
    }

    private bool SplittersMatch(LayoutDefinition layout)
    {
        int i = 0;
        foreach (int line in layout.VerticalSplitLines)
        {
            if (i >= Splitters.Count || !Splitters[i].IsVertical || Splitters[i].Line != line) return false;
            i++;
        }
        foreach (int line in layout.HorizontalSplitLines)
        {
            if (i >= Splitters.Count || Splitters[i].IsVertical || Splitters[i].Line != line) return false;
            i++;
        }
        return i == Splitters.Count;
    }

    private void UpdateSumNote(LayoutDefinition layout, IReadOnlyList<(int Width, int Height)> sizes,
                               int monW, int monH)
    {
        LayoutGeometry.Tracks tracks = LayoutGeometry.ComputeTracks(layout, sizes);
        int sumW = tracks.ColumnWidths.Sum();
        int sumH = tracks.RowHeights.Sum();

        var parts = new List<string>(2);
        if (sumW != monW) parts.Add($"widths sum to {sumW} px (monitor is {monW})");
        if (sumH != monH) parts.Add($"heights sum to {sumH} px (monitor is {monH})");

        SumNote = parts.Count == 0
            ? string.Empty
            : string.Join("; ", parts) + " — segments will be scaled into their on-screen rects.";
    }

    // ------------------------------------------------------------------ splitter dragging

    /// <summary>
    /// Begins an auto-fill drag. The current track sizes are first normalised so they sum to
    /// the monitor dimension exactly — this is how manual (non-summing) values re-enter
    /// auto-fill mode.
    /// </summary>
    public void BeginSplitterDrag(int splitterId, double canvasX, double canvasY)
    {
        LayoutDefinition? layout = SelectedLayout;
        MonitorInfo? monitor = SelectedMonitor;
        if (layout is null || monitor is null || IsBusy) return;

        SplitterVisual? splitter = Splitters.FirstOrDefault(s => s.Id == splitterId);
        if (splitter is null) return;

        var sizes = Segments.Select(s => (s.Width, s.Height)).ToList();
        LayoutGeometry.Tracks tracks = LayoutGeometry.ComputeTracks(layout, sizes);

        int monW = Math.Max(1, monitor.CurrentWidth);
        int monH = Math.Max(1, monitor.CurrentHeight);

        int[] cols = LayoutGeometry.Normalize(tracks.ColumnWidths, monW);
        int[] rows = LayoutGeometry.Normalize(tracks.RowHeights, monH);
        WriteTracks(layout, cols, rows);

        _dragSplitter = splitter;
        _dragTracks = splitter.IsVertical ? cols : rows;

        RebuildEditor();

        double linePos = splitter.IsVertical
            ? splitter.X + SplitterVisual.Thickness / 2
            : splitter.Y + SplitterVisual.Thickness / 2;
        _dragGrabOffset = (splitter.IsVertical ? canvasX : canvasY) - linePos;
    }

    /// <summary>
    /// Moves the captured splitter to an absolute canvas position. Working from the absolute
    /// pointer position (rather than accumulating per-move deltas) means clamping at the
    /// 640 px limit cannot desynchronise the bar from the cursor.
    /// </summary>
    public void UpdateSplitterDrag(double canvasX, double canvasY)
    {
        SplitterVisual? splitter = _dragSplitter;
        LayoutDefinition? layout = SelectedLayout;
        MonitorInfo? monitor = SelectedMonitor;
        if (splitter is null || layout is null || monitor is null) return;
        if (_dragTracks.Length < splitter.Line + 2) return;

        double canvasExtent = splitter.IsVertical ? CanvasWidth : CanvasHeight;
        int monitorExtent = splitter.IsVertical
            ? Math.Max(1, monitor.CurrentWidth)
            : Math.Max(1, monitor.CurrentHeight);
        if (canvasExtent <= 0) return;

        double targetCanvas = (splitter.IsVertical ? canvasX : canvasY) - _dragGrabOffset;
        double targetMonitor = targetCanvas * (monitorExtent / canvasExtent);

        int before = 0;
        for (int i = 0; i < splitter.Line; i++) before += _dragTracks[i];

        int pairTotal = _dragTracks[splitter.Line] + _dragTracks[splitter.Line + 1];
        int leading = LayoutGeometry.ClampSplit((int)Math.Round(targetMonitor - before), pairTotal);
        int trailing = pairTotal - leading;

        var updated = (int[])_dragTracks.Clone();
        updated[splitter.Line] = leading;
        updated[splitter.Line + 1] = trailing;

        if (splitter.IsVertical) WriteTracks(layout, updated, null);
        else WriteTracks(layout, null, updated);

        RebuildEditor();
        RaiseCommandStates();
    }

    /// <summary>Ends the drag. Values are already snapped into the text boxes by WriteTracks.</summary>
    public void EndSplitterDrag()
    {
        if (_dragSplitter is null) return;
        _dragSplitter = null;
        _dragTracks = Array.Empty<int>();
        RebuildEditor();
        RaiseCommandStates();
    }

    public bool IsDragging => _dragSplitter is not null;

    /// <summary>
    /// Pushes track sizes back into the segment editors (and therefore the text boxes).
    /// A null array leaves that axis untouched.
    /// </summary>
    private void WriteTracks(LayoutDefinition layout, int[]? cols, int[]? rows)
    {
        _editorSuspended = true;
        try
        {
            for (int i = 0; i < layout.SegmentCount && i < Segments.Count; i++)
            {
                LayoutCell cell = layout.Cells[i];
                int w = cols is null ? Segments[i].Width : SumRange(cols, cell.Col, cell.LastCol);
                int h = rows is null ? Segments[i].Height : SumRange(rows, cell.Row, cell.LastRow);
                Segments[i].Set(w, h);
            }
        }
        finally { _editorSuspended = false; }
    }

    private static int SumRange(int[] tracks, int first, int last)
    {
        int sum = 0;
        for (int i = first; i <= last && i < tracks.Length; i++) sum += tracks[i];
        return Math.Max(1, sum);
    }

    // ------------------------------------------------------------------ apply / revert

    /// <summary>User-initiated apply — always confirmed with the keep-or-revert countdown.</summary>
    public Task ApplyAsync() => ApplyAsync(promptToConfirm: true);

    public async Task ApplyAsync(bool promptToConfirm)
    {
        if (!CanApply) return;

        MonitorInfo physical = SelectedMonitor!;
        LayoutDefinition layout = SelectedLayout!;
        var resolutions = Segments.Select(s => (s.Width, s.Height)).ToList();

        var request = new ApplyRequest
        {
            Physical = physical,
            Layout = layout,
            Resolutions = resolutions,
            HidePhysicalDisplay = Hiding.IntentOn,
            SpecializationUi = Hiding.CreateSpecializationUi(() => Monitors.ToList()),
            PromptToConfirm = promptToConfirm,
            Confirmer = _applyConfirmer,
        };

        IsBusy = true;
        AppendStatus($"--- Applying '{layout.Name}' to {physical.DisplayLabel} ({physical.DeviceName}) ---");
        try
        {
            ApplyResult result = await Task.Run(() => ApplyService.Apply(request, _progress, CancellationToken.None))
                                           .ConfigureAwait(true);
            AppendStatus(result.RolledBack
                ? $"Change reverted — restored {result.RollbackDescription}."
                : $"Done. {result.VirtualMonitors.Count} virtual monitor(s) active.");

            if (!result.RolledBack) StartSuspensionWatcher();
        }
        catch (ApplyException ex)
        {
            AppendStatus("Apply failed: " + ex.Message);
        }
        catch (DriverNotFoundException ex)
        {
            AppendStatus("Apply failed: " + ex.Message);
        }
        catch (DriverIoException ex)
        {
            AppendStatus("Apply failed: " + ex.Message);
        }
        catch (DisplayArrangeException ex)
        {
            AppendStatus("Apply failed: " + ex.Message);
        }
        catch (CompositorNotFoundException ex)
        {
            AppendStatus("Apply failed: " + ex.Message);
        }
        catch (Exception ex)
        {
            AppendStatus("Apply failed (unexpected): " + ex.Message);
        }
        finally
        {
            IsBusy = false;
            LoadMonitors(announce: false);
            RefreshDriverStatus();
            RefreshCompositorStatus();
            _ = Hiding.RefreshAsync();
        }
    }

    public async Task RevertAsync()
    {
        IsBusy = true;
        AppendStatus("--- Reverting ---");

        // Drop any pending suspend state first: Revert restores primary itself, and a watcher
        // still reacting to events would fight it.
        StopSuspensionWatcher(restorePrimary: false);

        try
        {
            await Task.Run(() => ApplyService.Revert(_progress, CancellationToken.None)).ConfigureAwait(true);
        }
        catch (ApplyException ex)
        {
            AppendStatus("Revert failed: " + ex.Message);
        }
        catch (Exception ex)
        {
            AppendStatus("Revert failed (unexpected): " + ex.Message);
        }
        finally
        {
            IsBusy = false;
            LoadMonitors(announce: false);
            RefreshDriverStatus();
            RefreshCompositorStatus();
            _ = Hiding.RefreshAsync();
        }
    }

    /// <summary>Tray "Apply last": re-applies the persisted settings without touching the UI selection.</summary>
    public async Task ApplyLastAsync()
    {
        AppSettings? settings = SettingsStore.Load();
        if (settings is null || settings.Segments.Count == 0)
        {
            AppendStatus("No previously applied configuration found.");
            return;
        }
        if (!RestoreSettings(settings))
        {
            AppendStatus($"Monitor '{settings.PhysicalDevice}' from the saved settings is not connected.");
            return;
        }
        await ApplyAsync().ConfigureAwait(true);
    }

    // ------------------------------------------------------------------ status helpers

    public void RefreshDriverStatus()
    {
        try
        {
            if (!DriverClient.TryFindDevicePath(out string path))
            {
                DriverStatus = DriverClient.DriverMissingMessage;
                return;
            }
            try
            {
                DESKSPLIT_STATUS status = DriverClient.GetStatus();
                DriverStatus = $"Driver OK (protocol v{status.Version}, {status.ActiveMonitorCount} virtual monitor(s)).";
            }
            catch (DriverIoException)
            {
                DriverStatus = $"Driver device found ({path}) but GET_STATUS failed.";
            }
        }
        catch (Exception ex)
        {
            DriverStatus = "Driver check failed: " + ex.Message;
        }
    }

    public void RefreshCompositorStatus()
    {
        if (CompositorLauncher.IsRunning())
        {
            CompositorStatus = "Compositor: running.";
        }
        else if (CompositorLauncher.TryFindExecutable(out string exe))
        {
            CompositorStatus = $"Compositor: stopped ({exe}).";
        }
        else
        {
            CompositorStatus = $"Compositor: not found — expected {CompositorLauncher.ExpectedPath}.";
        }
    }

    public void AppendStatus(string line)
    {
        lock (_statusLock)
        {
            string stamped = $"[{DateTime.Now:HH:mm:ss}] {line}";
            StatusText = string.IsNullOrEmpty(StatusText) ? stamped : StatusText + Environment.NewLine + stamped;
        }
        StatusAppended?.Invoke(this, EventArgs.Empty);
    }

    public event EventHandler? StatusAppended;

    private void RaiseCommandStates()
    {
        OnPropertyChanged(nameof(CanApply));
        ApplyCommand.RaiseCanExecuteChanged();
        RevertCommand.RaiseCanExecuteChanged();
        RefreshMonitorsCommand.RaiseCanExecuteChanged();
        ResetSegmentsCommand.RaiseCanExecuteChanged();
    }

    // ------------------------------------------------------------------ settings

    private void RestoreSettings() => RestoreSettings(SettingsStore.Load());

    private bool RestoreSettings(AppSettings? settings)
    {
        if (settings is null) return false;

        MonitorInfo? monitor = Monitors.FirstOrDefault(
                                   m => string.Equals(m.DeviceName, settings.PhysicalDevice, StringComparison.OrdinalIgnoreCase))
                               ?? Monitors.FirstOrDefault(
                                   m => string.Equals(m.FriendlyName, settings.PhysicalFriendlyName, StringComparison.OrdinalIgnoreCase));
        if (monitor is null) return false;

        _suppressSegmentRebuild = true;
        try
        {
            _selectedMonitor = monitor;
            _selectedLayout = LayoutDefinition.FromKind(settings.Layout);
        }
        finally { _suppressSegmentRebuild = false; }

        OnPropertyChanged(nameof(SelectedMonitor));
        OnPropertyChanged(nameof(SelectedLayout));
        UpdateModeTexts();
        RebuildSegments(resetSizes: true);

        if (settings.Segments.Count == Segments.Count)
        {
            _editorSuspended = true;
            try
            {
                for (int i = 0; i < Segments.Count; i++)
                    Segments[i].Set(settings.Segments[i].Width, settings.Segments[i].Height);
            }
            finally { _editorSuspended = false; }

            RebuildEditor();
            AppendStatus($"Restored last settings from {SettingsStore.FilePath}.");
        }

        RaiseCommandStates();
        return true;
    }
}

