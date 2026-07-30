using DesktopSplitter.Models;

namespace DesktopSplitter.Services;

/// <summary>
/// Remembers the 64-bit monitor identity hash that Windows passes to
/// <c>SystemSettingsAdminFlows.exe SpecializeDisplay</c>, per monitor.
///
/// The derivation is not known, so instead of computing the hash we OBSERVE it: the first time
/// the user removes a display from the desktop through Windows Settings, the app reads the
/// helper's command line and remembers the value. From then on the same display can be hidden and
/// restored in one click.
///
/// Safety rule, enforced here rather than left to callers: a hash is bound to the exact monitor
/// identity it was captured from. There is no fallback, no guessing and no cross-monitor reuse —
/// a wrong hash risks putting a display into a specialized state the Settings UI cannot undo.
/// </summary>
public static class SpecializationHashStore
{
    /// <summary>
    /// Hashes captured on known hardware and shipped so those machines work out of the box.
    /// Keyed by device interface path; only ever matched against that exact string.
    /// </summary>
    private static readonly IReadOnlyDictionary<string, ulong> Seeds =
        new Dictionary<string, ulong>(StringComparer.OrdinalIgnoreCase)
        {
            // Philips 49M2C8900 — captured from a real Settings toggle:
            //   SystemSettingsAdminFlows.exe SpecializeDisplay 81569 0 260 1 8196125158964431964
            [@"\\?\DISPLAY#PHLC310#7&224ecef0&0&UID260#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}"] =
                8196125158964431964UL,
        };

    /// <summary>The identity a hash is keyed by. Empty when the monitor has no usable identity.</summary>
    public static string KeyFor(MonitorInfo monitor) => monitor.MonitorDevicePath ?? string.Empty;

    /// <summary>The learned hash for this exact monitor, or null.</summary>
    public static ulong? TryGet(MonitorInfo monitor)
    {
        string key = KeyFor(monitor);
        if (string.IsNullOrEmpty(key)) return null;

        AppSettings? settings = SettingsStore.Load();
        if (settings is not null)
        {
            foreach (KeyValuePair<string, string> entry in settings.SpecializationHashes)
            {
                if (!string.Equals(entry.Key, key, StringComparison.OrdinalIgnoreCase)) continue;
                if (ulong.TryParse(entry.Value, out ulong stored) && stored != 0) return stored;
            }
        }

        return Seeds.TryGetValue(key, out ulong seeded) ? seeded : null;
    }

    public static bool HasHash(MonitorInfo monitor) => TryGet(monitor) is not null;

    /// <summary>Records a hash observed for this exact monitor. Refuses zero.</summary>
    public static bool Learn(MonitorInfo monitor, ulong hash, IProgress<string>? log = null)
    {
        string key = KeyFor(monitor);
        if (string.IsNullOrEmpty(key) || hash == 0) return false;

        try
        {
            AppSettings settings = SettingsStore.Load() ?? new AppSettings();
            settings.SpecializationHashes ??= new Dictionary<string, string>();

            string? existing = settings.SpecializationHashes.Keys
                .FirstOrDefault(k => string.Equals(k, key, StringComparison.OrdinalIgnoreCase));
            if (existing is not null) settings.SpecializationHashes.Remove(existing);

            settings.SpecializationHashes[key] = hash.ToString();
            SettingsStore.Save(settings);

            log?.Report($"Learned the identity hash for {monitor.DisplayLabel}: {hash}. " +
                        "Display hiding can now be turned on and off automatically for this monitor.");
            return true;
        }
        catch (Exception ex)
        {
            log?.Report("Could not save the learned identity hash: " + ex.Message);
            return false;
        }
    }

    /// <summary>
    /// Copies any built-in seed that applies to a currently connected monitor into settings.json,
    /// so the value is visible and editable rather than hidden in the binary.
    /// </summary>
    public static void SeedKnownHashes(IEnumerable<MonitorInfo> monitors, IProgress<string>? log = null)
    {
        try
        {
            AppSettings settings = SettingsStore.Load() ?? new AppSettings();
            settings.SpecializationHashes ??= new Dictionary<string, string>();
            bool changed = false;

            foreach (MonitorInfo monitor in monitors)
            {
                string key = KeyFor(monitor);
                if (string.IsNullOrEmpty(key)) continue;
                if (!Seeds.TryGetValue(key, out ulong seeded)) continue;

                bool present = settings.SpecializationHashes.Keys
                    .Any(k => string.Equals(k, key, StringComparison.OrdinalIgnoreCase));
                if (present) continue;

                settings.SpecializationHashes[key] = seeded.ToString();
                changed = true;
                log?.Report($"Using the known identity hash for {monitor.DisplayLabel} ({seeded}).");
            }

            if (changed) SettingsStore.Save(settings);
        }
        catch
        {
            // Seeding is a convenience; never let it break startup.
        }
    }
}
