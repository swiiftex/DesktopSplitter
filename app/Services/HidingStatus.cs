using DesktopSplitter.Interop;
using DesktopSplitter.Models;
using Microsoft.Win32;

namespace DesktopSplitter.Services;

/// <summary>State of a status row.</summary>
public enum CheckState
{
    /// <summary>Could not be determined.</summary>
    Unknown,

    /// <summary>A capability or setting that is satisfied.</summary>
    Ok,

    /// <summary>A capability or setting that is NOT satisfied.</summary>
    No,

    /// <summary>
    /// Purely informational — neither good nor bad. Used for "Currently hidden: no", which is the
    /// normal resting state and must not read as a problem.
    /// </summary>
    Info,
}

public sealed record StatusRow(string Label, CheckState State, string Detail);

/// <summary>Everything the display-hiding panel shows, gathered in one pass.</summary>
public sealed record HidingSnapshot(
    StatusRow SystemSupport,
    StatusRow MonitorSupport,
    StatusRow CurrentlySpecialized,
    StatusRow Intent,
    bool SplitActive,
    bool Available,
    bool EnabledNow,
    bool IntentOn)
{
    public IReadOnlyList<StatusRow> Rows => new[] { SystemSupport, MonitorSupport, CurrentlySpecialized, Intent };
}

/// <summary>
/// Read-only probes behind the "Display hiding" panel, built on DisplayConfig monitor
/// specialization (GET, type 12) — the supported mechanism that Settings &gt; Advanced display
/// uses. Nothing here changes display state.
/// </summary>
public static class HidingStatus
{
    private const string DeskSplitKey = @"SOFTWARE\DesktopSplitter";
    private const string HideFlagValue = "HidePhysicalDisplay";

    public static HidingSnapshot Query(MonitorInfo? selected)
    {
        bool intentOn = ReadIntent();
        StatusRow intent = intentOn
            ? new StatusRow("应用时隐藏", CheckState.Ok,
                "开——应用分屏后会把物理显示器从桌面移除。")
            : new StatusRow("应用时隐藏", CheckState.No,
                "关——合成器只是覆盖物理显示器（默认）。");

        if (selected is null) return Degenerate("未选择显示器。", intent, intentOn);

        if (!MonitorSpecialization.TryGet(selected, out SpecializationState state, out int rc))
        {
            string detail = $"DisplayConfig GET（类型 12）失败，错误 {rc}" +
                            (rc == MonitorSpecialization.ERROR_INVALID_PARAMETER
                                ? "——此 Windows 版本可能不支持显示器专用化。"
                                : "。");
            return Degenerate(detail, intent, intentOn);
        }

        // Capability rows say what is POSSIBLE. The state row says what is HAPPENING. Keeping the
        // two visibly distinct matters: a green "supports hiding" was previously misread as
        // "already hiding".
        var system = new StatusRow("Windows 可隐藏显示器",
            state.AvailableForSystem ? CheckState.Ok : CheckState.No,
            state.AvailableForSystem
                ? "是——此 Windows 版本允许把显示器从桌面移除。"
                : "否——此 Windows 版本不允许把显示器从桌面移除。");

        var monitor = new StatusRow("此显示器可被隐藏",
            state.AvailableForMonitor ? CheckState.Ok : CheckState.No,
            state.AvailableForMonitor
                ? $"是——{selected.DisplayLabel} 符合条件（仅表示能力；GET 原始值 0x{state.RawValue:X}）。"
                : $"否——{selected.DisplayLabel} 不符合条件（GET 原始值 0x{state.RawValue:X}）。");

        var current = new StatusRow("当前已隐藏",
            state.EnabledNow ? CheckState.Ok : CheckState.Info,
            state.EnabledNow
                ? $"是——{selected.DisplayLabel} 当前已不在桌面上。"
                : $"否——{selected.DisplayLabel} 当前是普通的桌面显示器。");

        return new HidingSnapshot(system, monitor, current, intent,
                                  IsSplitActive(), state.Available, state.EnabledNow, intentOn);
    }

    private static HidingSnapshot Degenerate(string detail, StatusRow intent, bool intentOn)
        => new(new StatusRow("Windows 可隐藏显示器", CheckState.Unknown, detail),
               new StatusRow("此显示器可被隐藏", CheckState.Unknown, detail),
               new StatusRow("当前已隐藏", CheckState.Unknown, detail),
               intent, IsSplitActive(), false, false, intentOn);

    /// <summary>A split must be reverted before hiding can be enabled or disabled.</summary>
    public static bool IsSplitActive()
    {
        try
        {
            if (DisplayConfig.Enumerate().Any(m => m.IsDeskSplitVirtual)) return true;
        }
        catch (DisplayConfigException)
        {
            // Fall through to the compositor check.
        }
        return CompositorLauncher.IsRunning();
    }

    // ------------------------------------------------------------------ intent flag

    /// <summary>
    /// Reads the "hide during Apply" intent. HKLM wins when present (the installer or an admin
    /// may have set it, and it is what a standalone dscomp run reads); otherwise the per-user
    /// settings file, which never needs elevation.
    /// </summary>
    public static bool ReadIntent()
    {
        int? machine = ReadDword(Registry.LocalMachine, DeskSplitKey, HideFlagValue);
        if (machine is not null) return machine.Value != 0;
        return SettingsStore.Load()?.HidePhysicalDisplay ?? false;
    }

    public sealed record IntentWriteResult(bool Success, bool MachineWideWritten, string Message);

    /// <summary>
    /// Records the intent. The per-user copy always succeeds; the HKLM mirror is best-effort and
    /// its failure is reported, never worked around by elevating.
    /// </summary>
    public static IntentWriteResult WriteIntent(bool enabled)
    {
        AppSettings settings = SettingsStore.Load() ?? new AppSettings();
        settings.HidePhysicalDisplay = enabled;
        try
        {
            SettingsStore.Save(settings);
        }
        catch (Exception ex)
        {
            return new IntentWriteResult(false, false, "无法保存设置：" + ex.Message);
        }

        try
        {
            using RegistryKey key = Registry.LocalMachine.CreateSubKey(DeskSplitKey, writable: true)
                                   ?? throw new UnauthorizedAccessException("无法打开注册表项");
            key.SetValue(HideFlagValue, enabled ? 1 : 0, RegistryValueKind.DWord);
            return new IntentWriteResult(true, true,
                $"显示器隐藏已{(enabled ? "启用" : "禁用")}（已为当前用户和整机保存）。");
        }
        catch (Exception ex) when (ex is UnauthorizedAccessException or System.Security.SecurityException)
        {
            return new IntentWriteResult(true, false,
                $"显示器隐藏已为当前用户{(enabled ? "启用" : "禁用")}。" +
                $@"整机标志（HKLM\{DeskSplitKey}\{HideFlagValue}）需要管理员" +
                "权限，未做修改——DesktopSplitter 会自行给合成器传递 --specialized 参数，" +
                "所以这只在你单独运行 dscomp.exe 时才有影响。");
        }
        catch (Exception ex)
        {
            return new IntentWriteResult(true, false,
                $"显示器隐藏已为当前用户{(enabled ? "启用" : "禁用")}；整机" +
                $"标志写入失败：{ex.Message}");
        }
    }

    private static int? ReadDword(RegistryKey root, string subKey, string name)
    {
        try
        {
            using RegistryKey? key = root.OpenSubKey(subKey);
            return key?.GetValue(name) is int value ? value : null;
        }
        catch
        {
            return null;
        }
    }
}
