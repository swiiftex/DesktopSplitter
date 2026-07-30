using System.Runtime.InteropServices;
using System.Text;
using DesktopSplitter.Models;
using Microsoft.Win32;

namespace DesktopSplitter.Services;

public sealed record TaskbarPreferenceResult(bool Changed, bool RestartNeeded, string Message);

/// <summary>
/// Manages Windows' "show my taskbar on all displays" preference
/// (HKCU\...\Explorer\Advanced\MMTaskbarEnabled), which decides whether non-primary displays get
/// a taskbar at all.
///
/// MEASURED on Windows 11 build 26200: changing this value does NOT take effect on a running
/// Explorer. Toggling it and broadcasting WM_SETTINGCHANGE("TraySettings"),
/// WM_SETTINGCHANGE("Environment") and SHChangeNotify left the secondary taskbar count unchanged
/// for 5+ seconds. Explorer only reads it at startup, so a restart (or sign-out) is required.
/// We therefore never restart Explorer silently — we set the value, tell the user, and let them
/// decide. Because the value is sticky, leaving it at 1 is the practical steady state.
///
/// What a SECONDARY taskbar actually contains on this build, measured through UI Automation:
///   * Start button, pinned and running app buttons  — present
///   * A clock                                        — present (a plain group, not the primary's
///                                                       interactive SystemTray.OmniButton)
///   * System tray icons (network, volume, "Show Hidden Icons") — ABSENT
///   * The notification centre affordance             — ABSENT
/// So a non-primary segment can show app buttons and the time, but never the tray or the
/// notification centre; those follow the primary display.
/// </summary>
public static class MultiMonitorTaskbar
{
    private const string AdvancedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced";
    private const string ValueName = "MMTaskbarEnabled";

    /// <summary>Current value, or null when the value has never been set.</summary>
    public static int? Read()
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(AdvancedKey);
            return key?.GetValue(ValueName) is int value ? value : null;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>True when Windows is currently configured to show taskbars on all displays.</summary>
    public static bool IsEnabled() => (Read() ?? 1) != 0;

    /// <summary>How many secondary taskbars Explorer has actually created right now.</summary>
    public static int CountSecondaryTaskbars()
    {
        int count = 0;
        try
        {
            EnumWindows((hwnd, _) =>
            {
                var sb = new StringBuilder(64);
                if (GetClassName(hwnd, sb, sb.Capacity) > 0 && sb.ToString() == "Shell_SecondaryTrayWnd")
                    count++;
                return true;
            }, IntPtr.Zero);
        }
        catch
        {
            // Best effort; only used to decide whether to nag about restarting Explorer.
        }
        return count;
    }

    /// <summary>
    /// Ensures the preference matches <paramref name="wanted"/>, remembering the previous value the
    /// first time we change it so Revert can put it back. Reports whether Explorer needs
    /// restarting for the change to become visible.
    /// </summary>
    public static TaskbarPreferenceResult Ensure(bool wanted, IProgress<string>? log = null)
    {
        int? current = Read();
        bool currentlyOn = (current ?? 1) != 0;

        if (currentlyOn == wanted)
        {
            return new TaskbarPreferenceResult(false, false,
                $"Taskbars on all displays already {(wanted ? "enabled" : "disabled")}.");
        }

        try
        {
            RememberPrevious(current);

            using RegistryKey key = Registry.CurrentUser.CreateSubKey(AdvancedKey, true)
                                   ?? throw new InvalidOperationException("Explorer Advanced key unavailable.");
            key.SetValue(ValueName, wanted ? 1 : 0, RegistryValueKind.DWord);
            Broadcast();

            // Explorer caches this at startup; see the class comment for the measurement.
            bool visible = wanted ? CountSecondaryTaskbars() > 0 : CountSecondaryTaskbars() == 0;
            string message = wanted
                ? "Turned on Windows' \"show my taskbar on all displays\"."
                : "Turned off Windows' \"show my taskbar on all displays\".";

            if (!visible)
            {
                message += " Windows only reads this when Explorer starts, so it will not appear " +
                           "until you sign out and back in (or restart Explorer). DesktopSplitter " +
                           "will not restart Explorer for you.";
            }

            log?.Report(message);
            return new TaskbarPreferenceResult(true, !visible, message);
        }
        catch (Exception ex)
        {
            string message = "Could not change the multi-display taskbar setting: " + ex.Message;
            log?.Report(message);
            return new TaskbarPreferenceResult(false, false, message);
        }
    }

    /// <summary>Puts the preference back to whatever it was before we first changed it.</summary>
    public static void RestorePrevious(IProgress<string>? log = null)
    {
        try
        {
            AppSettings? settings = SettingsStore.Load();
            if (settings?.PreviousMultiMonitorTaskbar is null) return;

            int previous = settings.PreviousMultiMonitorTaskbar.Value;
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(AdvancedKey, true)!)
            {
                if (previous < 0) key.DeleteValue(ValueName, false);
                else key.SetValue(ValueName, previous, RegistryValueKind.DWord);
            }
            Broadcast();

            settings.PreviousMultiMonitorTaskbar = null;
            SettingsStore.Save(settings);

            log?.Report(previous < 0
                ? "Restored the multi-display taskbar setting (removed the value we added)."
                : $"Restored the multi-display taskbar setting to {previous}.");
        }
        catch (Exception ex)
        {
            log?.Report("Could not restore the multi-display taskbar setting: " + ex.Message);
        }
    }

    /// <summary>Stores the pre-change value once, using -1 to mean "the value did not exist".</summary>
    private static void RememberPrevious(int? current)
    {
        try
        {
            AppSettings settings = SettingsStore.Load() ?? new AppSettings();
            if (settings.PreviousMultiMonitorTaskbar is not null) return;   // already remembered
            settings.PreviousMultiMonitorTaskbar = current ?? -1;
            SettingsStore.Save(settings);
        }
        catch
        {
            // If we cannot remember it we simply will not restore it; never block the change.
        }
    }

    /// <summary>Best-effort notifications. Measured not to be sufficient, but harmless and correct.</summary>
    private static void Broadcast()
    {
        try
        {
            SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, IntPtr.Zero, "TraySettings",
                               SMTO_ABORTIFHUNG, 2000, out _);
            SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, IntPtr.Zero, "Environment",
                               SMTO_ABORTIFHUNG, 2000, out _);
        }
        catch
        {
            // Broadcasting is advisory.
        }
    }

    private const int WM_SETTINGCHANGE = 0x001A;
    private const uint SMTO_ABORTIFHUNG = 0x0002;
    private static readonly IntPtr HWND_BROADCAST = new(0xFFFF);

    private delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SendMessageTimeout(
        IntPtr hWnd, int msg, IntPtr wParam, string lParam, uint flags, uint timeout, out IntPtr result);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumWindows(EnumProc callback, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hwnd, StringBuilder name, int maxCount);
}
