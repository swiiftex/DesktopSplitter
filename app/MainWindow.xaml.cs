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

    private sealed class CountdownHandle : ICountdownHandle
    {
        private readonly CountdownWindow _window;

        public CountdownHandle(CountdownWindow window) => _window = window;

        public void CloseFromHelper() => _window.CloseFromHelper();
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
        // Closing the window keeps the app alive in the tray; Exit is explicit.
        if (Application.Current is App { IsExiting: false } && _tray is not null)
        {
            e.Cancel = true;
            HideToTray();
            return;
        }
        base.OnClosing(e);
    }
}
