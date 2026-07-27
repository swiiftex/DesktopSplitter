using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Threading;
using DesktopSplitter.Models;

namespace DesktopSplitter.Views;

/// <summary>
/// The keep-or-revert safety dialog. The countdown here is cosmetic — the elevated helper owns
/// the real timer and will auto-revert on its own — so a stalled UI can never strand the display.
/// </summary>
public partial class CountdownWindow : Window
{
    private readonly DispatcherTimer _timer;
    private DateTime _deadline;
    private bool _answered;

    public CountdownWindow(int timeoutSeconds)
    {
        InitializeComponent();

        TimeoutSeconds = Math.Max(1, timeoutSeconds);
        _deadline = DateTime.UtcNow.AddSeconds(TimeoutSeconds);

        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        _timer.Tick += (_, _) => UpdateCountdown();
        UpdateCountdown();
    }

    public int TimeoutSeconds { get; }

    /// <summary>True when the user pressed Keep, false when they pressed Revert, null on timeout.</summary>
    public bool? Decision { get; private set; }

    /// <summary>Raised when the user answers. Argument is true for keep.</summary>
    public event Action<bool>? Answered;

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        _timer.Start();
        Activate();
    }

    /// <summary>
    /// Positions the window on <paramref name="monitor"/> using PHYSICAL pixels, sidestepping
    /// WPF's DIP coordinates entirely — Left/Top are unreliable across monitors with different
    /// scaling, and this dialog must land precisely on the non-target display.
    /// </summary>
    public void PlaceOn(MonitorInfo monitor)
    {
        var helper = new WindowInteropHelper(this);
        IntPtr hwnd = helper.Handle;
        if (hwnd == IntPtr.Zero)
        {
            helper.EnsureHandle();
            hwnd = helper.Handle;
        }
        if (hwnd == IntPtr.Zero) return;

        // Ask the window how big it actually is in physical pixels, then centre that.
        if (!GetWindowRect(hwnd, out RECT current)) return;
        int width = Math.Max(1, current.right - current.left);
        int height = Math.Max(1, current.bottom - current.top);

        PixelRect target = CountdownPlacement.Center(monitor, width, height);

        SetWindowPos(hwnd, HWND_TOPMOST, target.X, target.Y, target.W, target.H,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    private void UpdateCountdown()
    {
        double remaining = (_deadline - DateTime.UtcNow).TotalSeconds;
        if (remaining < 0) remaining = 0;

        CountdownText.Text = Math.Ceiling(remaining).ToString("0");

        if (remaining <= 0 && !_answered)
        {
            _timer.Stop();
            CountdownCaption.Text = "reverting…";
            KeepButton.IsEnabled = false;
            RevertButton.IsEnabled = false;
        }
    }

    /// <summary>Closes the dialog because the helper finished on its own.</summary>
    public void CloseFromHelper()
    {
        _timer.Stop();
        _answered = true;
        try { Close(); } catch (InvalidOperationException) { /* already closing */ }
    }

    private void Keep_Click(object sender, RoutedEventArgs e) => Answer(true);

    private void Revert_Click(object sender, RoutedEventArgs e) => Answer(false);

    private void Answer(bool keep)
    {
        if (_answered) return;
        _answered = true;
        _timer.Stop();
        Decision = keep;

        KeepButton.IsEnabled = false;
        RevertButton.IsEnabled = false;
        CountdownCaption.Text = keep ? "keeping…" : "reverting…";

        Answered?.Invoke(keep);
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
