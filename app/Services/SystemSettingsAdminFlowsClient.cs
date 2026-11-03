using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Threading;
using DesktopSplitter.Interop;
using DesktopSplitter.Models;

namespace DesktopSplitter.Services;

public sealed record AdminFlowResult(bool Success, string Message)
{
    /// <summary>True when we did not even try, so the caller should fall back rather than report failure.</summary>
    public bool NotAttempted { get; init; }
}

/// <summary>
/// Drives Windows' own elevated helper for "Remove display from desktop":
///
///   SystemSettingsAdminFlows.exe SpecializeDisplay &lt;adapterLow&gt; &lt;adapterHigh&gt; &lt;targetId&gt; &lt;enable&gt; &lt;hash&gt;
///
/// This is the command line SettingsHandlers_PCDisplay.dll builds when the user flips the toggle.
/// Shelling out to it is deliberately preferred over crafting the private type -23 DisplayConfig
/// packet ourselves: Microsoft's binary owns the privileged part, and it is far less fragile
/// across Windows versions.
///
/// The 5th argument is a hash of the monitor's stable identity (see <see cref="MonitorIdentityHash"/>).
/// The exact recipe is not yet proven, so this path stays DISABLED until a captured real-world
/// hash has been reproduced offline and the winning recipe recorded in settings. Until then
/// <see cref="TryRun"/> reports <see cref="AdminFlowResult.NotAttempted"/> and the guided
/// Settings flow handles everything.
/// </summary>
public static class SystemSettingsAdminFlowsClient
{
    public const string ExecutableName = "SystemSettingsAdminFlows.exe";
    public const string Verb = "SpecializeDisplay";

    public static string ExecutablePath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), ExecutableName);

    public static bool IsAvailable => File.Exists(ExecutablePath);

    /// <summary>True when this exact monitor has a known-good identity hash.</summary>
    public static bool CanDrive(MonitorInfo monitor) => IsAvailable && SpecializationHashStore.HasHash(monitor);

    /// <summary>
    /// Builds the exact argument list Windows itself uses. Returns null unless a hash was
    /// captured for THIS monitor — never derived, never defaulted, never borrowed.
    /// </summary>
    public static IReadOnlyList<string>? BuildArguments(MonitorInfo monitor, bool enable)
    {
        ulong? hash = SpecializationHashStore.TryGet(monitor);
        if (hash is null || hash.Value == 0) return null;

        return BuildArguments(monitor, enable, hash.Value);
    }

    /// <summary>Builds the argument list for an explicit hash (used by the round-trip test).</summary>
    public static IReadOnlyList<string> BuildArguments(MonitorInfo monitor, bool enable, ulong hash)
        => new[]
        {
            Verb,
            monitor.AdapterIdLow.ToString(),
            monitor.AdapterIdHigh.ToString(),
            monitor.TargetId.ToString(),
            enable ? "1" : "0",
            hash.ToString(),
        };

    /// <summary>
    /// Runs the helper elevated (one UAC prompt) and waits for it. Returns NotAttempted when the
    /// hash recipe has not been proven yet, so the caller falls through to the guided flow.
    /// </summary>
    public static AdminFlowResult TryRun(
        MonitorInfo monitor, bool enable, IProgress<string> log, CancellationToken ct)
    {
        if (!IsAvailable)
        {
            return new AdminFlowResult(false, $"找不到 {ExecutablePath}。") { NotAttempted = true };
        }

        IReadOnlyList<string>? args = BuildArguments(monitor, enable);
        if (args is null)
        {
            return new AdminFlowResult(false,
                $"尚未学到 {monitor.DisplayLabel} 的标识哈希，因此无法对它使用一键方式。")
            { NotAttempted = true };
        }

        var psi = new ProcessStartInfo
        {
            FileName = ExecutablePath,
            UseShellExecute = true,   // required for the runas verb
            Verb = "runas",
        };
        foreach (string a in args) psi.ArgumentList.Add(a);

        log.Report($"正在运行：{ExecutableName} {string.Join(" ", args)}   （已提权——会出现 UAC 提示）");

        try
        {
            using Process? process = Process.Start(psi);
            if (process is null)
                return new AdminFlowResult(false, "助手程序未能启动。");

            if (!process.WaitForExit((int)TimeSpan.FromSeconds(60).TotalMilliseconds))
                return new AdminFlowResult(false, "助手程序未在 60 秒内完成。");

            return process.ExitCode == 0
                ? new AdminFlowResult(true,
                    enable ? $"已把 {monitor.DisplayLabel} 从桌面移除。"
                           : $"已把 {monitor.DisplayLabel} 恢复到桌面。")
                : new AdminFlowResult(false, $"助手程序退出，代码 {process.ExitCode}。");
        }
        catch (Win32Exception ex) when (ex.NativeErrorCode == 1223)   // ERROR_CANCELLED
        {
            return new AdminFlowResult(false, "提权提示被取消。");
        }
        catch (Exception ex)
        {
            return new AdminFlowResult(false, $"无法运行 {ExecutableName}：{ex.Message}");
        }
    }
}
