using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Threading;
using DesktopSplitter.Models;
using DesktopSplitter.Services;
using DesktopSplitter.ViewModels;
using DesktopSplitter.Views;

namespace DesktopSplitter;

public partial class MainWindow : Window, IHidingUi
{
    private const int WM_DISPLAYCHANGE = 0x007E;

    private readonly MainViewModel _viewModel;
    private readonly TrayIconService? _tray;
    private readonly DispatcherTimer _displayChangeDebounce;

    public MainWindow() : this(new MainViewModel(), null) { }

    public MainWindow(MainViewModel viewModel, TrayIconService? tray)
    {
        _viewModel = viewModel;
        _tray = tray;

        InitializeComponent();
        DataContext = _viewModel;

        // Windows fires WM_DISPLAYCHANGE several times per topology change.
        _displayChangeDebounce = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(600) };
        _displayChangeDebounce.Tick += (_, _) =>
        {
            _displayChangeDebounce.Stop();
            _viewModel.OnDisplayChanged();
        };

        _viewModel.MinimizeToTrayRequested += (_, _) => HideToTray();
        _viewModel.StatusAppended += (_, _) => Dispatcher.BeginInvoke(new Action(() => StatusBox.ScrollToEnd()));

        _viewModel.Hiding.AttachUi(this);
        _viewModel.AttachConfirmer(new ApplyConfirmer(this, _viewModel.AppendStatus));
        _viewModel.LoadPreferences();

        Title = string.IsNullOrEmpty(MainViewModel.VersionText)
            ? "DesktopSplitter"
            : $"DesktopSplitter {MainViewModel.VersionText}";
        VersionLabel.Text = MainViewModel.VersionText;
    }

    // ------------------------------------------------------------------ IHidingUi

    public bool Confirm(string title, string message)
        => MessageBox.Show(this, message, title, MessageBoxButton.OKCancel, MessageBoxImage.Warning,
                           MessageBoxResult.Cancel) == MessageBoxResult.OK;

    public void ShowOutcome(string title, string message, bool success)
        => MessageBox.Show(this, message, title, MessageBoxButton.OK,
                           success ? MessageBoxImage.Information : MessageBoxImage.Warning);

    public ICountdownHandle ShowCountdown(int seconds, MonitorInfo? placeOn, Action onKeep, Action onRevert)
    {
        var window = new CountdownWindow(seconds);
        window.Answered += keep =>
        {
            if (keep) onKeep(); else onRevert();
        };

        // Show first so the handle exists, then move it in physical pixels.
        window.Show();
        if (placeOn is not null) window.PlaceOn(placeOn);
        window.Activate();

        return new CountdownHandle(window);
    }

    public ICountdownHandle ShowApplyCountdown(string summary, int seconds, Action onKeep, Action onRevert)
    {
        var window = new CountdownWindow(
            seconds,
            heading: "Keep these display settings?",
            detail: $"Applied: {summary}." + Environment.NewLine +
                    "If anything looks wrong, or you cannot see this clearly, do nothing — " +
                    "the previous settings come back when the countdown reaches zero.");

        window.Answered += keep =>
        {
            if (keep) onKeep(); else onRevert();
        };

        window.Show();
        window.PlaceOnPrimary();
        window.Activate();
        window.Focus();

        return new CountdownHandle(window);
    }

    private sealed class CountdownHandle : ICountdownHandle
    {
        private readonly CountdownWindow _window;

        public CountdownHandle(CountdownWindow window) => _window = window;

        public void CloseFromHelper() => _window.CloseFromHelper();
    }

    public IGuidedHandle ShowGuidedSettings(
        MonitorInfo target, bool wantEnabled, string reason, MonitorInfo? placeOn, Action<bool> onFinished)
    {
        var window = new ManualSpecializationWindow(target, wantEnabled, reason);
        window.Finished += onFinished;

        window.Show();
        if (placeOn is not null) window.PlaceOn(placeOn);
        window.Activate();

        // Take the user straight there — the button is for a second look.
        ManualSpecializationWindow.OpenDisplaySettings();

        return new GuidedHandle(window);
    }

    private sealed class GuidedHandle : IGuidedHandle
    {
        private readonly ManualSpecializationWindow _window;

        public GuidedHandle(ManualSpecializationWindow window) => _window = window;

        public void CloseFromCaller() => _window.CloseFromCaller();
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        if (PresentationSource.FromVisual(this) is HwndSource source)
            source.AddHook(WndProc);
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WM_DISPLAYCHANGE)
        {
            _displayChangeDebounce.Stop();
            _displayChangeDebounce.Start();
        }
        return IntPtr.Zero;
    }

    // ------------------------------------------------------------------ splitter dragging

    private void Splitter_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is not Border border || border.Tag is not int id) return;

        Point p = e.GetPosition(EditorSurface);
        _viewModel.BeginSplitterDrag(id, p.X, p.Y);
        border.CaptureMouse();
        e.Handled = true;
    }

    private void Splitter_MouseMove(object sender, MouseEventArgs e)
    {
        if (sender is not Border border || !border.IsMouseCaptured) return;
        if (e.LeftButton != MouseButtonState.Pressed)
        {
            border.ReleaseMouseCapture();
            return;
        }

        Point p = e.GetPosition(EditorSurface);
        _viewModel.UpdateSplitterDrag(p.X, p.Y);
        e.Handled = true;
    }

    private void Splitter_MouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (sender is not Border border) return;
        if (border.IsMouseCaptured) border.ReleaseMouseCapture();
        _viewModel.EndSplitterDrag();
        e.Handled = true;
    }

    private void Splitter_LostMouseCapture(object sender, MouseEventArgs e)
        => _viewModel.EndSplitterDrag();

    // ------------------------------------------------------------------ window lifetime

    private void HideToTray()
    {
        Hide();
        _tray?.ShowBalloon("DesktopSplitter",
            "Still running in the tray. Right-click the icon for Apply last / Revert / Exit.");
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        // Closing the window normally keeps the app alive in the tray; Exit is explicit. When the
        // user has turned that off, closing the window really exits — and App's teardown reverts.
        bool minimiseToTray = SettingsStore.Load()?.MinimizeToTrayOnClose ?? true;

        if (Application.Current is App { IsExiting: false } && _tray is not null && minimiseToTray)
        {
            e.Cancel = true;
            HideToTray();
            return;
        }

        base.OnClosing(e);
        if (Application.Current is App app && !app.IsExiting) app.ExitApplication();
    }
}
