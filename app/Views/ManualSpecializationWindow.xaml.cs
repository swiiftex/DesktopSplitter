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
            HeadingText.Text = "在 Windows 设置中操作一步";
            StepsText.Text =
                "1.  点击下方的\"打开显示设置\"。\n" +
                $"2.  在\"选择要查看或更改其设置的显示器\"下，选择 {name}。\n" +
                "3.  打开\"从桌面移除显示器\"。\n\n" +
                "屏幕会黑屏片刻，然后你的分割就会出现在该显示器上。";
            StatusText.Text = $"正在等待 {target.DisplayLabel} 离开桌面…";
        }
        else
        {
            HeadingText.Text = "将显示器恢复到桌面";
            StepsText.Text =
                "1.  点击下方的\"打开显示设置\"。\n" +
                $"2.  选择 {name}。\n" +
                "3.  关闭\"从桌面移除显示器\"。";
            StatusText.Text = $"正在等待 {target.DisplayLabel} 返回桌面…";
            OpenSettingsButton.Content = "打开显示设置";
            CancelButton.Content = "关闭";
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
            StatusText.Text = $"无法读取显示器状态（错误 {rc}）。" +
                              "你仍可在「设置」中更改该开关。";
            return;
        }

        if (state.EnabledNow == _wantEnabled)
        {
            Succeeded = true;
            Finish(true);
            return;
        }

        StatusText.Text = _wantEnabled
            ? $"正在等待 {_target.DisplayLabel} 离开桌面…（当前位于桌面上）"
            : $"正在等待 {_target.DisplayLabel} 返回桌面…（当前已隐藏）";
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
