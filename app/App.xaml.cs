using System.Threading;
using System.Windows;
using DesktopSplitter.Services;
using DesktopSplitter.ViewModels;
using Microsoft.Win32;

namespace DesktopSplitter;

public partial class App : Application
{
    private TrayIconService? _tray;
    private MainViewModel? _viewModel;
    private MainWindow? _window;
    private int _teardownDone;

    internal bool IsExiting { get; private set; }

    /// <summary>True when Windows auto-started us from the Run key.</summary>
    internal bool LaunchedAtStartup { get; private set; }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        LaunchedAtStartup = StartupRegistration.LaunchedAtStartup(e.Args);

        DispatcherUnhandledException += (_, args) =>
        {
            MessageBox.Show(
                "Unexpected error:" + Environment.NewLine + args.Exception.Message,
                "DesktopSplitter", MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };

        // Last-resort teardown: a crash must not leave the desktop split with nothing driving it.
        AppDomain.CurrentDomain.UnhandledException += (_, _) => Teardown();
        AppDomain.CurrentDomain.ProcessExit += (_, _) => Teardown();

        // Logoff / shutdown / restart.
        SystemEvents.SessionEnding += (_, _) => Teardown();

        _viewModel = new MainViewModel();

        _tray = new TrayIconService();
        _tray.OpenRequested += (_, _) => ShowMainWindow();
        _tray.ApplyLastRequested += async (_, _) =>
        {
            ShowMainWindow();
            await _viewModel.ApplyLastAsync();
        };
        _tray.RevertRequested += async (_, _) => await _viewModel.RevertAsync();
        _tray.ExitRequested += (_, _) => ExitApplication();

        _window = new MainWindow(_viewModel, _tray);

        // Auto-started at logon: go straight to the tray rather than popping the window open.
        if (!LaunchedAtStartup) _window.Show();

        _ = _viewModel.AutoApplyOnStartupAsync();
    }

    private void ShowMainWindow()
    {
        if (_window is null) return;
        _window.Show();
        if (_window.WindowState == WindowState.Minimized)
            _window.WindowState = WindowState.Normal;
        _window.Activate();
    }

    internal void ExitApplication()
    {
        IsExiting = true;
        Teardown();
        _window?.Close();
        Shutdown();
    }

    /// <summary>
    /// Reverts the split on the way out, whichever exit path we took. Idempotent, because several
    /// of these paths can fire for a single shutdown.
    /// </summary>
    private void Teardown()
    {
        if (Interlocked.Exchange(ref _teardownDone, 1) != 0) return;
        try { _viewModel?.RevertOnExit(); } catch { /* shutdown must proceed */ }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        IsExiting = true;
        Teardown();

        _tray?.Dispose();
        _tray = null;
        base.OnExit(e);
    }
}
