using System.Drawing;
using System.Drawing.Drawing2D;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace DesktopSplitter.Services;

/// <summary>
/// Tray presence backed by WinForms <see cref="NotifyIcon"/> (UseWindowsForms is enabled
/// alongside UseWPF purely for this) — no third-party tray package.
/// </summary>
public sealed class TrayIconService : IDisposable
{
    private readonly NotifyIcon _notifyIcon;
    private readonly ContextMenuStrip _menu;
    private readonly ToolStripMenuItem _applyLastItem;
    private readonly ToolStripMenuItem _revertItem;
    private Icon? _generatedIcon;
    private IntPtr _generatedIconHandle;
    private bool _disposed;

    public TrayIconService()
    {
        _applyLastItem = new ToolStripMenuItem("Apply last configuration");
        _applyLastItem.Click += (_, _) => ApplyLastRequested?.Invoke(this, EventArgs.Empty);

        _revertItem = new ToolStripMenuItem("Revert");
        _revertItem.Click += (_, _) => RevertRequested?.Invoke(this, EventArgs.Empty);

        var openItem = new ToolStripMenuItem("Open DesktopSplitter") { Font = new Font(SystemFonts.MenuFont ?? SystemFonts.DefaultFont, FontStyle.Bold) };
        openItem.Click += (_, _) => OpenRequested?.Invoke(this, EventArgs.Empty);

        var exitItem = new ToolStripMenuItem("Exit");
        exitItem.Click += (_, _) => ExitRequested?.Invoke(this, EventArgs.Empty);

        _menu = new ContextMenuStrip();
        _menu.Items.Add(openItem);
        _menu.Items.Add(new ToolStripSeparator());
        _menu.Items.Add(_applyLastItem);
        _menu.Items.Add(_revertItem);
        _menu.Items.Add(new ToolStripSeparator());
        _menu.Items.Add(exitItem);

        _notifyIcon = new NotifyIcon
        {
            Icon = CreateIcon(),
            Text = "DesktopSplitter",
            Visible = true,
            ContextMenuStrip = _menu,
        };
        _notifyIcon.DoubleClick += (_, _) => OpenRequested?.Invoke(this, EventArgs.Empty);
    }

    public event EventHandler? OpenRequested;
    public event EventHandler? ApplyLastRequested;
    public event EventHandler? RevertRequested;
    public event EventHandler? ExitRequested;

    public bool ApplyLastEnabled
    {
        get => _applyLastItem.Enabled;
        set => _applyLastItem.Enabled = value;
    }

    public bool RevertEnabled
    {
        get => _revertItem.Enabled;
        set => _revertItem.Enabled = value;
    }

    public void ShowBalloon(string title, string text)
    {
        if (_disposed) return;
        _notifyIcon.BalloonTipTitle = title;
        _notifyIcon.BalloonTipText = text;
        _notifyIcon.BalloonTipIcon = ToolTipIcon.Info;
        _notifyIcon.ShowBalloonTip(3000);
    }

    /// <summary>Draws a tiny "split monitor" glyph so we do not ship a binary .ico resource.</summary>
    private Icon CreateIcon()
    {
        try
        {
            using var bmp = new Bitmap(32, 32);
            using (Graphics g = Graphics.FromImage(bmp))
            {
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.Clear(Color.Transparent);

                using var frame = new Pen(Color.FromArgb(230, 235, 245), 2f);
                using var left = new SolidBrush(Color.FromArgb(76, 141, 255));
                using var right = new SolidBrush(Color.FromArgb(120, 200, 160));

                g.FillRectangle(left, 4, 7, 11, 18);
                g.FillRectangle(right, 17, 7, 11, 18);
                g.DrawRectangle(frame, 3, 6, 26, 20);
            }

            _generatedIconHandle = bmp.GetHicon();
            using var temp = Icon.FromHandle(_generatedIconHandle);
            _generatedIcon = (Icon)temp.Clone();
            return _generatedIcon;
        }
        catch
        {
            return SystemIcons.Application;
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        _notifyIcon.Visible = false;
        _notifyIcon.Dispose();
        _menu.Dispose();
        _generatedIcon?.Dispose();
        if (_generatedIconHandle != IntPtr.Zero)
        {
            DestroyIcon(_generatedIconHandle);
            _generatedIconHandle = IntPtr.Zero;
        }
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyIcon(IntPtr hIcon);
}
