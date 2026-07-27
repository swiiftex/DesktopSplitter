using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Threading;
using DesktopSplitter.Interop;
using DesktopSplitter.Models;

namespace DesktopSplitter.Services;

public sealed class ApplyException : Exception
{
    public ApplyException(string message) : base(message) { }
    public ApplyException(string message, Exception inner) : base(message, inner) { }
}

public sealed class ApplyRequest
{
    public required MonitorInfo Physical { get; init; }
    public required LayoutDefinition Layout { get; init; }

    /// <summary>One entry per layout segment, in layout order.</summary>
    public required IReadOnlyList<(int Width, int Height)> Resolutions { get; init; }
}

public sealed class ApplyResult
{
    public required IReadOnlyList<MonitorInfo> VirtualMonitors { get; init; }
    public required string ConfigPath { get; init; }
    public required int CompositorPid { get; init; }
}

/// <summary>
/// Orchestrates the full apply / revert sequence described in docs/ARCHITECTURE.md.
/// All methods are synchronous and blocking; call them off the UI thread and pass an
/// <see cref="IProgress{T}"/> created on the UI thread for status updates.
/// </summary>
public static class ApplyService
{
    public const int MinSegmentDimension = LayoutGeometry.MinSegmentDimension;
    public const int MaxSegmentDimension = LayoutGeometry.MaxSegmentDimension;

    private static readonly TimeSpan MonitorArrivalTimeout = TimeSpan.FromSeconds(15);
    private static readonly TimeSpan CompositorStopTimeout = TimeSpan.FromSeconds(10);
    private static readonly TimeSpan PollInterval = TimeSpan.FromMilliseconds(400);
    private static readonly TimeSpan EdidMappingRetryTimeout = TimeSpan.FromSeconds(3);

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    /// <summary>%ProgramData%\DesktopSplitter</summary>
    public static string ConfigDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "DesktopSplitter");

    public static string ConfigPath => Path.Combine(ConfigDirectory, "config.json");

    // ------------------------------------------------------------------ apply

    public static ApplyResult Apply(ApplyRequest request, IProgress<string> log, CancellationToken ct)
    {
        ValidateRequest(request);

        MonitorInfo physical = request.Physical;
        LayoutDefinition layout = request.Layout;
        int segmentCount = layout.SegmentCount;

        // 0. A compositor from a previous run must not be holding duplication handles on
        //    monitors we are about to tear down.
        if (CompositorLauncher.IsRunning())
        {
            log.Report("Stopping the running compositor first...");
            CompositorLauncher.StopAndWait(CompositorStopTimeout, log.Report);
        }

        // Baseline: everything that is NOT one of our virtual monitors.
        IReadOnlyList<MonitorInfo> before = DisplayConfig.Enumerate();
        var baseline = before
            .Where(m => !m.IsDeskSplitVirtual)
            .Select(m => m.DeviceName)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);

        // Remember which display was primary BEFORE we touch anything — Revert restores
        // exactly this monitor, which is not necessarily the one being split.
        (string previousPrimaryDevice, string previousPrimaryName) = CapturePreviousPrimary(before, log);

        // a. Build and send DESKSPLIT_CONFIG.
        DESKSPLIT_CONFIG cfg = DriverClient.NewConfig();
        cfg.MonitorCount = (uint)segmentCount;
        for (int i = 0; i < segmentCount; i++)
        {
            (int w, int h) = request.Resolutions[i];
            cfg.Monitors[i].ModeCount = 1;
            cfg.Monitors[i].PreferredModeIndex = 0;
            cfg.Monitors[i].Modes[0] = new DESKSPLIT_MODE
            {
                Width = (uint)w,
                Height = (uint)h,
                RefreshMillihertz = physical.MaxRefreshMillihertz,
            };
        }

        log.Report($"Sending IOCTL_DESKSPLIT_SET_CONFIG (0x{DriverClient.IOCTL_DESKSPLIT_SET_CONFIG:X8}) " +
                   $"for {segmentCount} monitor(s) at {physical.MaxRefreshMillihertz} mHz...");
        try
        {
            DriverClient.SetConfig(cfg);
        }
        catch (DriverNotFoundException ex)
        {
            throw new ApplyException(ex.Message, ex);
        }
        catch (DriverIoException ex)
        {
            throw new ApplyException(ex.Message, ex);
        }

        // b. Wait for the virtual monitors to arrive.
        log.Report($"Waiting for {segmentCount} virtual monitor(s) to appear (up to {MonitorArrivalTimeout.TotalSeconds:0}s)...");
        IReadOnlyList<MonitorInfo> arrived = WaitForVirtualMonitors(baseline, segmentCount, ct);
        foreach (MonitorInfo m in arrived)
        {
            log.Report($"  Found {m.DeviceName} ({m.FriendlyName})" +
                       (m.Edid is null ? "  [no EDID]" : $"  [{m.Edid}]"));
        }

        // Map segment i -> the monitor the driver created as DESKSPLIT_CONFIG.Monitors[i],
        // identified by the EDID serial the driver stamps (0xD5000001 + index).
        IReadOnlyList<MonitorInfo> virtualMonitors = MapSegmentsToMonitors(baseline, arrived, segmentCount, log, ct);

        // c. Arrange them contiguously to the right of the physical monitor.
        log.Report("Arranging virtual monitors in the virtual desktop...");
        IReadOnlyList<DisplayArranger.VirtualPlacement> placements =
            DisplayArranger.ComputePlacements(physical, layout, virtualMonitors, request.Resolutions);
        DisplayArranger.Apply(physical, placements, physical.MaxRefreshHz, log.Report);

        // Re-read so config.json records what Windows actually settled on, keeping the
        // segment -> monitor mapping established above.
        var refreshed = new Dictionary<string, MonitorInfo>(StringComparer.OrdinalIgnoreCase);
        foreach (MonitorInfo m in SafeEnumerate()) refreshed[m.DeviceName] = m;
        virtualMonitors = virtualMonitors
            .Select(v => refreshed.TryGetValue(v.DeviceName, out MonitorInfo? cur) ? cur : v)
            .ToList();

        // c2. Promote segment 1's virtual monitor to PRIMARY. The shell anchors the Start
        //     menu, Alt-Tab switcher and most centred dialogs to the primary display; leaving
        //     the physical monitor primary drew them across the whole panel, behind/around
        //     the compositor's output instead of inside a segment.
        string appliedPrimaryDevice = virtualMonitors[0].DeviceName;
        SwitchPrimary(appliedPrimaryDevice, $"segment 1 ({appliedPrimaryDevice})", log);

        // d. Write %ProgramData%\DesktopSplitter\config.json.
        // physRects mirror the editor: track sizes are normalised onto the monitor's real
        // dimensions (last track absorbs the rounding remainder) rather than being an equal
        // subdivision, so a dragged/typed split lands on screen exactly where it was drawn.
        IReadOnlyList<PixelRect> physRects = LayoutGeometry.ComputePhysRects(
            layout, physical.CurrentWidth, physical.CurrentHeight, request.Resolutions);
        log.Report("Segment rects on the physical monitor: " +
                   string.Join("  ", physRects.Select((r, i) => $"[{i + 1}] {r}")));
        var compositorConfig = new CompositorConfig
        {
            PhysicalDevice = physical.DeviceName,
            RefreshMillihertz = physical.MaxRefreshMillihertz,
        };
        for (int i = 0; i < segmentCount; i++)
        {
            (int w, int h) = request.Resolutions[i];
            compositorConfig.Segments.Add(new CompositorSegment
            {
                VirtualDevice = virtualMonitors[i].DeviceName,
                Width = w,
                Height = h,
                PhysRect = CompositorRect.From(physRects[i]),
            });
        }
        WriteCompositorConfig(compositorConfig);
        log.Report($"Wrote {ConfigPath}");

        // e. Launch the compositor.
        Process compositor = CompositorLauncher.Start();
        log.Report($"Started {compositor.ProcessName}.exe (pid {compositor.Id}).");

        // f. Remember the settings for next launch / tray "Apply last".
        SettingsStore.Save(new AppSettings
        {
            PhysicalDevice = physical.DeviceName,
            PhysicalFriendlyName = physical.FriendlyName,
            Layout = layout.Kind,
            Segments = request.Resolutions.Select(r => new SegmentSize { Width = r.Width, Height = r.Height }).ToList(),
            PreviousPrimaryDevice = previousPrimaryDevice,
            PreviousPrimaryFriendlyName = previousPrimaryName,
            AppliedPrimaryDevice = appliedPrimaryDevice,
            LastAppliedUtc = DateTime.UtcNow,
        });
        log.Report($"Saved {SettingsStore.FilePath}");

        return new ApplyResult
        {
            VirtualMonitors = virtualMonitors,
            ConfigPath = ConfigPath,
            CompositorPid = compositor.Id,
        };
    }

    // ------------------------------------------------------------------ revert

    public static void Revert(IProgress<string> log, CancellationToken ct)
    {
        log.Report("Stopping the compositor...");
        bool stopped = CompositorLauncher.StopAndWait(CompositorStopTimeout, log.Report);
        log.Report(stopped ? "Compositor stopped." : "Compositor may still be running.");

        // Hand primary back to a physical monitor BEFORE the virtual ones disappear. If the
        // primary vanishes, Windows picks an arbitrary survivor and re-bases the desktop on
        // its own terms, which scrambles window positions.
        RestorePrimaryBeforeTeardown(log);

        log.Report("Sending IOCTL_DESKSPLIT_SET_CONFIG with MonitorCount = 0...");
        try
        {
            DriverClient.ClearConfig();
        }
        catch (DriverNotFoundException ex)
        {
            throw new ApplyException(ex.Message, ex);
        }
        catch (DriverIoException ex)
        {
            throw new ApplyException(ex.Message, ex);
        }

        // Best effort: give Windows a moment to retire the virtual monitors.
        DateTime deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
        while (DateTime.UtcNow < deadline && !ct.IsCancellationRequested)
        {
            if (SafeEnumerate().All(m => !m.IsDeskSplitVirtual)) break;
            ct.WaitHandle.WaitOne(PollInterval);
        }

        log.Report("Reverted — virtual monitors removed.");
    }

    // ------------------------------------------------------------------ primary display

    private static bool SameDevice(string? a, string? b)
        => !string.IsNullOrEmpty(a) && !string.IsNullOrEmpty(b) &&
           string.Equals(a, b, StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Determines which display should be restored as primary on Revert.
    /// If a split is already active (one of our virtual monitors currently holds primary),
    /// the value saved by the first Apply is carried forward rather than overwritten —
    /// otherwise a second Apply would "remember" a virtual monitor as the original primary.
    /// </summary>
    private static (string Device, string FriendlyName) CapturePreviousPrimary(
        IReadOnlyList<MonitorInfo> before, IProgress<string> log)
    {
        MonitorInfo? livePrimary = before.FirstOrDefault(m => m.IsPrimary);

        if (livePrimary is not null && !livePrimary.IsDeskSplitVirtual)
        {
            log.Report($"Current primary display: {livePrimary.DisplayLabel} ({livePrimary.DeviceName}) " +
                       "— will be restored on Revert.");
            return (livePrimary.DeviceName, livePrimary.FriendlyName);
        }

        AppSettings? prior = SettingsStore.Load();
        if (prior is not null && !string.IsNullOrEmpty(prior.PreviousPrimaryDevice))
        {
            log.Report("A split is already active; carrying forward the saved pre-split primary " +
                       $"{prior.PreviousPrimaryDevice}.");
            return (prior.PreviousPrimaryDevice, prior.PreviousPrimaryFriendlyName);
        }

        MonitorInfo? fallback = before.FirstOrDefault(m => !m.IsDeskSplitVirtual);
        if (fallback is null)
        {
            log.Report("WARNING: could not determine a physical display to restore as primary on Revert.");
            return (string.Empty, string.Empty);
        }

        log.Report($"No saved pre-split primary; will restore {fallback.DeviceName} on Revert.");
        return (fallback.DeviceName, fallback.FriendlyName);
    }

    /// <summary>
    /// Makes <paramref name="targetDevice"/> the primary display. Windows pins the primary at
    /// the virtual-desktop origin, so this re-bases every monitor by the same vector — all
    /// relative offsets, and therefore the arrangement, are preserved exactly.
    /// Never throws: a failure here leaves a working split with the wrong primary, which is
    /// far better than aborting the apply.
    /// </summary>
    private static bool SwitchPrimary(string targetDevice, string description, IProgress<string> log)
    {
        if (string.IsNullOrEmpty(targetDevice))
        {
            log.Report("WARNING: no target display for the primary switch; leaving it unchanged.");
            return false;
        }

        IReadOnlyList<MonitorInfo> current = SafeEnumerate();
        if (current.Count == 0)
        {
            log.Report("WARNING: could not enumerate displays; primary display left unchanged.");
            return false;
        }

        MonitorInfo? livePrimary = current.FirstOrDefault(m => m.IsPrimary);
        if (livePrimary is not null && SameDevice(livePrimary.DeviceName, targetDevice))
        {
            log.Report($"{description} is already the primary display.");
            return true;
        }

        IReadOnlyList<DisplayArranger.MonitorPlacement> snapshot = DisplayArranger.SnapshotLayout(current);
        DisplayArranger.MonitorPlacement? origin =
            snapshot.FirstOrDefault(p => SameDevice(p.DeviceName, targetDevice));
        if (origin is null)
        {
            log.Report($"WARNING: {targetDevice} is not in the current display set; primary left unchanged.");
            return false;
        }

        try
        {
            IReadOnlyList<DisplayArranger.MonitorPlacement> rebased =
                DisplayArranger.RebaseToPrimary(snapshot, targetDevice);

            log.Report($"Making {description} the primary display — re-basing all " +
                       $"{rebased.Count} monitor(s) by ({-origin.X},{-origin.Y}):");
            DisplayArranger.ApplyLayout(rebased, log.Report);
            return true;
        }
        catch (DisplayArrangeException ex)
        {
            log.Report("WARNING: primary switch failed — " + ex.Message);
            return false;
        }
    }

    /// <summary>
    /// Picks the best physical monitor to hold primary once the split is torn down and
    /// switches to it. Preference order: the display that was primary before Apply, then the
    /// monitor that was split, then whatever physical monitor is available.
    /// </summary>
    private static void RestorePrimaryBeforeTeardown(IProgress<string> log)
    {
        var physicalMonitors = SafeEnumerate().Where(m => !m.IsDeskSplitVirtual).ToList();
        if (physicalMonitors.Count == 0)
        {
            log.Report("WARNING: no physical monitors found; leaving the primary display alone.");
            return;
        }

        AppSettings? settings = SettingsStore.Load();
        MonitorInfo? target = null;
        string why = string.Empty;

        if (settings is not null)
        {
            target = physicalMonitors.FirstOrDefault(m => SameDevice(m.DeviceName, settings.PreviousPrimaryDevice));
            if (target is not null) why = "primary before the split";

            if (target is null && !string.IsNullOrWhiteSpace(settings.PreviousPrimaryFriendlyName))
            {
                target = physicalMonitors.FirstOrDefault(
                    m => SameDevice(m.FriendlyName, settings.PreviousPrimaryFriendlyName));
                if (target is not null) why = "primary before the split, matched by name";
            }

            if (target is null)
            {
                target = physicalMonitors.FirstOrDefault(m => SameDevice(m.DeviceName, settings.PhysicalDevice));
                if (target is not null) why = "the monitor that was split; no saved previous primary";
            }
        }

        if (target is null)
        {
            target = physicalMonitors.FirstOrDefault(m => m.IsPrimary) ?? physicalMonitors[0];
            why = "first available physical monitor; no usable saved state";
        }

        SwitchPrimary(target.DeviceName, $"{target.DisplayLabel} ({target.DeviceName}) [{why}]", log);
    }

    // ------------------------------------------------------------------ helpers

    public static void ValidateRequest(ApplyRequest request)
    {
        if (request.Resolutions.Count != request.Layout.SegmentCount)
        {
            throw new ApplyException(
                $"Layout '{request.Layout.Name}' needs {request.Layout.SegmentCount} segment resolutions, " +
                $"got {request.Resolutions.Count}.");
        }
        if (request.Layout.SegmentCount > DriverClient.DESKSPLIT_MAX_MONITORS)
        {
            throw new ApplyException(
                $"Layout '{request.Layout.Name}' needs {request.Layout.SegmentCount} monitors but the driver " +
                $"supports at most {DriverClient.DESKSPLIT_MAX_MONITORS}.");
        }
        for (int i = 0; i < request.Resolutions.Count; i++)
        {
            (int w, int h) = request.Resolutions[i];
            if (w < MinSegmentDimension || w > MaxSegmentDimension ||
                h < MinSegmentDimension || h > MaxSegmentDimension)
            {
                throw new ApplyException(
                    $"Segment {i + 1} resolution {w}x{h} is out of range " +
                    $"({MinSegmentDimension}..{MaxSegmentDimension}).");
            }
        }
        if (request.Physical.IsDeskSplitVirtual)
            throw new ApplyException("The selected monitor is a DesktopSplitter virtual monitor; pick a physical one.");
    }

    /// <summary>
    /// Orders the arrived virtual monitors so that index i is the monitor the driver created
    /// from DESKSPLIT_CONFIG.Monitors[i], using the EDID serial (0xD5000001 + index) that the
    /// driver stamps. Falls back to positional \\.\DISPLAYn order with a logged warning when
    /// the EDIDs are unreadable, incomplete or ambiguous.
    /// </summary>
    private static IReadOnlyList<MonitorInfo> MapSegmentsToMonitors(
        HashSet<string> baseline, IReadOnlyList<MonitorInfo> arrived, int segmentCount,
        IProgress<string> log, CancellationToken ct)
    {
        // Windows caches a monitor's EDID under its devnode shortly after arrival; on the
        // first read it may not be there yet. Retry with fresh enumerations (each one
        // re-reads the EDIDs) before giving up on identity mapping.
        DateTime deadline = DateTime.UtcNow + EdidMappingRetryTimeout;
        string reason;

        while (true)
        {
            if (TryMapByEdid(arrived, segmentCount, out List<MonitorInfo> mapped, out reason))
            {
                log.Report("Mapped segments to virtual monitors by EDID serial (driver monitor index).");
                for (int i = 0; i < mapped.Count; i++)
                {
                    EdidInfo edid = mapped[i].Edid!;
                    log.Report($"  Segment {i + 1} -> {mapped[i].DeviceName} " +
                               $"(serial 0x{edid.SerialNumber:X8}, product 0x{edid.ProductCode:X4})");
                    if (edid.ProductCode != i)
                    {
                        log.Report($"  Note: EDID product code 0x{edid.ProductCode:X4} does not match monitor " +
                                   $"index {i}; trusting the serial.");
                    }
                }
                return mapped;
            }

            if (DateTime.UtcNow >= deadline || ct.IsCancellationRequested) break;
            ct.WaitHandle.WaitOne(PollInterval);

            var refreshed = SafeEnumerate()
                .Where(m => m.IsDeskSplitVirtual || !baseline.Contains(m.DeviceName))
                .OrderBy(m => DisplayConfig.DeviceIndex(m.DeviceName))
                .ThenBy(m => m.DeviceName, StringComparer.OrdinalIgnoreCase)
                .ToList();
            if (refreshed.Count == arrived.Count) arrived = refreshed;
        }

        log.Report($"WARNING: {reason} — falling back to positional \\\\.\\DISPLAYn-order mapping. " +
                   "Segments may end up on the wrong virtual monitor.");
        return arrived;
    }

    private static bool TryMapByEdid(
        IReadOnlyList<MonitorInfo> arrived, int segmentCount,
        out List<MonitorInfo> mapped, out string reason)
    {
        var byIndex = new Dictionary<int, MonitorInfo>();
        bool duplicates = false;

        foreach (MonitorInfo m in arrived)
        {
            int index = m.DeskSplitMonitorIndex;
            if (index < 0 || index >= segmentCount) continue;
            if (!byIndex.TryAdd(index, m)) duplicates = true;
        }

        bool complete = !duplicates
                        && byIndex.Count == segmentCount
                        && Enumerable.Range(0, segmentCount).All(byIndex.ContainsKey);

        if (complete)
        {
            mapped = Enumerable.Range(0, segmentCount).Select(i => byIndex[i]).ToList();
            reason = string.Empty;
            return true;
        }

        mapped = new List<MonitorInfo>();
        reason = duplicates
            ? "two virtual monitors reported the same driver index"
            : byIndex.Count == 0
                ? "no EDID serial in the DesktopSplitter range could be read"
                : $"only {byIndex.Count} of {segmentCount} monitors reported a usable driver index";
        return false;
    }

    private static IReadOnlyList<MonitorInfo> WaitForVirtualMonitors(
        HashSet<string> baseline, int expected, CancellationToken ct)
    {
        DateTime deadline = DateTime.UtcNow + MonitorArrivalTimeout;
        List<MonitorInfo> found = new();

        while (DateTime.UtcNow < deadline)
        {
            ct.ThrowIfCancellationRequested();

            found = SafeEnumerate()
                .Where(m => m.IsDeskSplitVirtual || !baseline.Contains(m.DeviceName))
                .OrderBy(m => DisplayConfig.DeviceIndex(m.DeviceName))
                .ThenBy(m => m.DeviceName, StringComparer.OrdinalIgnoreCase)
                .ToList();

            if (found.Count == expected) return found;

            ct.WaitHandle.WaitOne(PollInterval);
        }

        throw new ApplyException(
            $"Timed out after {MonitorArrivalTimeout.TotalSeconds:0}s waiting for the virtual monitors: " +
            $"expected {expected}, saw {found.Count}. Is the DesktopSplitter driver installed and started " +
            "(Device Manager -> Display adapters)?");
    }

    private static IReadOnlyList<MonitorInfo> SafeEnumerate()
    {
        try { return DisplayConfig.Enumerate(); }
        catch (DisplayConfigException) { return Array.Empty<MonitorInfo>(); }
    }

    /// <summary>
    /// Writes config.json to %ProgramData%\DesktopSplitter. There is deliberately no
    /// fallback location: the compositor's contract is a single fixed path.
    /// </summary>
    public static void WriteCompositorConfig(CompositorConfig config)
    {
        try
        {
            Directory.CreateDirectory(ConfigDirectory);
            File.WriteAllText(ConfigPath, JsonSerializer.Serialize(config, JsonOptions));
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or System.Security.SecurityException)
        {
            throw new ApplyException(
                $"Access denied writing {ConfigPath}.{Environment.NewLine}" +
                "Run DesktopSplitter as administrator once — it will create " +
                $"{ConfigDirectory} and grant the current user write access; " +
                "subsequent runs do not need elevation.", ex);
        }
        catch (IOException ex)
        {
            throw new ApplyException($"Could not write {ConfigPath}: {ex.Message}", ex);
        }
    }
}
