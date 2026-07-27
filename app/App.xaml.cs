using System.Windows;
using DesktopSplitter.Services;
using DesktopSplitter.ViewModels;

namespace DesktopSplitter;

public partial class App : Application
{
    private TrayIconService? _tray;
    private MainViewModel? _viewModel;
    private MainWindow? _window;

    internal bool IsExiting { get; private set; }

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        DispatcherUnhandledException += (_, args) =>
        {
            MessageBox.Show(
                "Unexpected error:" + Environment.NewLine + args.Exception.Message,
                "DesktopSplitter", MessageBoxButton.OK, MessageBoxImage.Error);
            args.Handled = true;
        };

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
        _window.Show();
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
        _window?.Close();
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _tray?.Dispose();
        _tray = null;
        base.OnExit(e);
    }
}
