using System.Threading.Tasks;
using Windows.Devices.Display;
using Windows.Devices.Enumeration;

namespace DesktopSplitter.Interop;

/// <summary>How Windows classifies a monitor.</summary>
public enum MonitorUsage
{
    /// <summary>Could not be determined (WinRT unavailable, monitor not enumerated).</summary>
    Unknown = 0,

    /// <summary>An ordinary desktop display.</summary>
    Standard = 1,

    /// <summary>
    /// Removed from the desktop; only a DisplayManager client can drive it. This is what the
    /// EDID override is trying to achieve.
    /// </summary>
    SpecialPurpose = 2,
}

/// <summary>
/// Reads <c>Windows.Devices.Display.DisplayMonitor.UsageKind</c> through the WinRT projection
/// (hence the 10.0.19041.0 platform suffix on the TFM).
///
/// <see cref="DeviceInformation.Id"/> is byte-for-byte the same device interface path that
/// <c>DISPLAYCONFIG_TARGET_DEVICE_NAME.monitorDevicePath</c> yields, so entries key straight
/// onto <see cref="Models.MonitorInfo.MonitorDevicePath"/> with an ordinal-ignore-case compare.
/// </summary>
public static class DisplayUsage
{
    /// <summary>
    /// Snapshot of every present monitor's usage kind, keyed by device interface path.
    /// Never throws — an empty map simply means "unknown", which the UI reports honestly.
    /// </summary>
    public static async Task<IReadOnlyDictionary<string, MonitorUsage>> BuildMapAsync()
    {
        var map = new Dictionary<string, MonitorUsage>(StringComparer.OrdinalIgnoreCase);

        try
        {
            DeviceInformationCollection devices = await DeviceInformation
                .FindAllAsync(DisplayMonitor.GetDeviceSelector())
                .AsTask()
                .ConfigureAwait(false);

            foreach (DeviceInformation device in devices)
            {
                try
                {
                    DisplayMonitor? monitor = await DisplayMonitor
                        .FromInterfaceIdAsync(device.Id)
                        .AsTask()
                        .ConfigureAwait(false);
                    if (monitor is null) continue;

                    map[device.Id] = monitor.UsageKind switch
                    {
                        DisplayMonitorUsageKind.Standard => MonitorUsage.Standard,
                        DisplayMonitorUsageKind.SpecialPurpose => MonitorUsage.SpecialPurpose,
                        _ => MonitorUsage.Unknown,
                    };
                }
                catch
                {
                    // A single monitor failing to open must not blank the whole map.
                }
            }
        }
        catch
        {
            // WinRT unavailable / access denied: report unknown rather than crashing the panel.
        }

        return map;
    }

    /// <summary>Looks up one monitor's usage kind in a snapshot.</summary>
    public static MonitorUsage Lookup(IReadOnlyDictionary<string, MonitorUsage> map, string monitorDevicePath)
        => !string.IsNullOrEmpty(monitorDevicePath) && map.TryGetValue(monitorDevicePath, out MonitorUsage usage)
            ? usage
            : MonitorUsage.Unknown;
}
