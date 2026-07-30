using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;
using DesktopSplitter.Interop;
using DesktopSplitter.Models;

namespace DesktopSplitter.Views;

/// <summary>
/// Guides the user through Windows' own "Remove display from desktop" toggle, which drives the
/// same DisplayConfig monitor-specialization mechanism from a privileged context that our
/// in-process SET is refused from.
///
/// It polls the read-only GET probe and closes itself the moment the display actually changes
/// state, so the user never has to tell us they are done. It deliberately lives on a monitor
/// OTHER than the one being hidden — the target goes black while it is off the desktop.
/// </summary>
public partial class ManualSpecializationWindow : Window
{
    private readonly DispatcherTimer _poll;
    private readonly MonitorInfo _target;
    private readonly bool _wantEnabled;

    public ManualSpecializationWindow(MonitorInfo target, bool wantEnabled, string reason)
    {
        InitializeComponent();

        _target = target;
        _wantEnabled = wantEnabled;

        string name = $"{target.DisplayLabel} ({target.DeviceName})";

        if (wantEnabled)
        {
            HeadingText.Text = "One step in Windows Settings";
            StepsText.Text =
                "1.  Press \"Open display settings\" below.\n" +
                $"2.  Under \"Select a display to view or change its settings\", choose {name}.\n" +
                "3.  Turn ON \"Remove display from desktop\".\n\n" +
                "The screen will go black for a moment, then your split appears on it.";
            StatusText.Text = $"Waiting for {target.DisplayLabel} to leave the desktop...";
        }
        else
        {
            HeadingText.Text = "Put the display back";
            StepsText.Text =
                "1.  Press \"Open display settings\" below.\n" +
                $"2.  Choose {name}.\n" +
                "3.  Turn OFF \"Remove display from desktop\".";
            StatusText.Text = $"Waiting for {target.DisplayLabel} to return to the desktop...";
            OpenSettingsButton.Content = "Open display settings";
            CancelButton.Content = "Close";
        }

        ReasonText.Text = reason;

        _poll = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        _poll.Tick += (_, _) => Poll();
    }

    /// <summary>True once the display reached the state we were waiting for.</summary>
    public bool Succeeded { get; private set; }

    /// <summary>Raised when the wait finishes — true if the display changed, false if cancelled.</summary>
    public event Action<bool>? Finished;

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        _poll.Start();
        Poll();
        Activate();
    }

    /// <summary>Positions the window on <paramref name="monitor"/> in physical pixels.</summary>
    public void PlaceOn(MonitorInfo monitor)
    {
        var helper = new WindowInteropHelper(this);
        if (helper.Handle == IntPtr.Zero) helper.EnsureHandle();
        IntPtr hwnd = helper.Handle;
        if (hwnd == IntPtr.Zero) return;

        if (!GetWindowRect(hwnd, out RECT current)) return;
        int width = Math.Max(1, current.right - current.left);
        int height = Math.Max(1, current.bottom - current.top);

        PixelRect target = CountdownPlacement.Center(monitor, width, height);
        SetWindowPos(hwnd, HWND_TOPMOST, target.X, target.Y, target.W, target.H,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    private void Poll()
    {
        if (!MonitorSpecialization.TryGet(_target, out SpecializationState state, out int rc))
        {
            StatusText.Text = $"Could not read the display state (error {rc}). " +
                              "You can still change the toggle in Settings.";
            return;
        }

        if (state.EnabledNow == _wantEnabled)
        {
            Succeeded = true;
            Finish(true);
            return;
        }

        StatusText.Text = _wantEnabled
            ? $"Waiting for {_target.DisplayLabel} to leave the desktop...  (currently on the desktop)"
            : $"Waiting for {_target.DisplayLabel} to return to the desktop...  (currently hidden)";
    }

    private void OpenSettings_Click(object sender, RoutedEventArgs e) => OpenDisplaySettings();

    /// <summary>Deep-links straight to Settings &gt; System &gt; Display &gt; Advanced display.</summary>
    public static void OpenDisplaySettings()
    {
        try
        {
            Process.Start(new ProcessStartInfo("ms-settings:display-advanced") { UseShellExecute = true });
        }
        catch
        {
            try
            {
                Process.Start(new ProcessStartInfo("ms-settings:display") { UseShellExecute = true });
            }
            catch { /* the instructions still name the page */ }
        }
    }

    private void Cancel_Click(object sender, RoutedEventArgs e) => Finish(false);

    /// <summary>Closes the dialog from outside (e.g. the compositor died).</summary>
    public void CloseFromCaller() => Finish(false);

    private bool _finished;

    private void Finish(bool success)
    {
        if (_finished) return;
        _finished = true;
        _poll.Stop();
        Succeeded = success;
        Finished?.Invoke(success);
        try { Close(); } catch (InvalidOperationException) { /* already closing */ }
    }

    // ------------------------------------------------------------------ P/Invoke

    private static readonly IntPtr HWND_TOPMOST = new(-1);
    private const uint SWP_NOACTIVATE = 0x0010;
    private const uint SWP_SHOWWINDOW = 0x0040;

    [StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct RECT
    {
        public int left, top, right, bottom;
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowPos(
        IntPtr hWnd, IntPtr hWndInsertAfter, int x, int y, int cx, int cy, uint uFlags);
}
