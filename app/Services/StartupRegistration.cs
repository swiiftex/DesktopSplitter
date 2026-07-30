using System.Diagnostics;
using System.IO;
using System.Reflection;
using Microsoft.Win32;

namespace DesktopSplitter.Services;

/// <summary>Where the Run entry currently points, and whether that is still us.</summary>
public sealed record StartupState(bool Enabled, string RegisteredCommand, bool PathMatchesCurrentExe)
{
    /// <summary>Registered, but pointing at a different build (e.g. dev output vs installed copy).</summary>
    public bool NeedsRefresh => Enabled && !PathMatchesCurrentExe;
}

/// <summary>
/// "Start with Windows", via HKCU\...\CurrentVersion\Run — per-user, so it never needs elevation.
///
/// The registered command always carries <see cref="StartupArgument"/> so the app can tell it was
/// auto-started (and therefore must not prompt for confirmation). The registry is the single
/// source of truth: the checkbox reads it live, so removing the entry with another tool shows up
/// correctly here.
/// </summary>
public static class StartupRegistration
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "DesktopSplitter";

    /// <summary>Command-line flag marking an auto-start launch.</summary>
    public const string StartupArgument = "--startup";

    /// <summary>The executable to register. Uses the real .exe, not the managed dll.</summary>
    public static string CurrentExecutablePath
    {
        get
        {
            try
            {
                string? main = Process.GetCurrentProcess().MainModule?.FileName;
                if (!string.IsNullOrEmpty(main) &&
                    main.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) &&
                    !Path.GetFileName(main).Equals("dotnet.exe", StringComparison.OrdinalIgnoreCase))
                {
                    return main;
                }
            }
            catch
            {
                // Fall through to the assembly-relative guess.
            }

            string name = Assembly.GetEntryAssembly()?.GetName().Name ?? "DesktopSplitter";
            return Path.Combine(AppContext.BaseDirectory, name + ".exe");
        }
    }

    /// <summary>The exact command we want in the Run key.</summary>
    public static string ExpectedCommand => $"\"{CurrentExecutablePath}\" {StartupArgument}";

    /// <summary>True when this process was launched by the Run entry.</summary>
    public static bool LaunchedAtStartup(IEnumerable<string> args)
        => args.Any(a => string.Equals(a, StartupArgument, StringComparison.OrdinalIgnoreCase));

    /// <summary>Reads the live registry state — never a cached copy.</summary>
    public static StartupState Read()
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(RunKeyPath);
            if (key?.GetValue(ValueName) is not string command || string.IsNullOrWhiteSpace(command))
                return new StartupState(false, string.Empty, false);

            return new StartupState(true, command, PointsAtCurrentExe(command));
        }
        catch
        {
            return new StartupState(false, string.Empty, false);
        }
    }

    public static bool IsEnabled() => Read().Enabled;

    /// <summary>Adds or updates the Run entry so it points at the running executable.</summary>
    public static bool Enable(IProgress<string>? log = null)
    {
        try
        {
            using RegistryKey key = Registry.CurrentUser.CreateSubKey(RunKeyPath, writable: true)
                                   ?? throw new InvalidOperationException("Run key could not be opened.");
            key.SetValue(ValueName, ExpectedCommand, RegistryValueKind.String);
            log?.Report($"DesktopSplitter will start with Windows ({ExpectedCommand}).");
            return true;
        }
        catch (Exception ex)
        {
            log?.Report("Could not enable start with Windows: " + ex.Message);
            return false;
        }
    }

    public static bool Disable(IProgress<string>? log = null)
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: true);
            if (key?.GetValue(ValueName) is not null)
            {
                key.DeleteValue(ValueName, throwOnMissingValue: false);
                log?.Report("DesktopSplitter will no longer start with Windows.");
            }
            return true;
        }
        catch (Exception ex)
        {
            log?.Report("Could not disable start with Windows: " + ex.Message);
            return false;
        }
    }

    public static bool Set(bool enabled, IProgress<string>? log = null)
        => enabled ? Enable(log) : Disable(log);

    /// <summary>
    /// If we are registered but the stored path is stale — the app was moved, or the user
    /// installed a copy after running a dev build — quietly rewrite it to the running exe.
    /// </summary>
    public static void RefreshIfStale(IProgress<string>? log = null)
    {
        StartupState state = Read();
        if (!state.NeedsRefresh) return;

        log?.Report($"Updating the start-with-Windows entry: it pointed at {state.RegisteredCommand}.");
        Enable(log);
    }

    /// <summary>True when the registered command launches the executable we are running from.</summary>
    private static bool PointsAtCurrentExe(string command)
    {
        string registered = ExtractExecutable(command);
        if (string.IsNullOrEmpty(registered)) return false;

        try
        {
            return string.Equals(
                Path.GetFullPath(registered),
                Path.GetFullPath(CurrentExecutablePath),
                StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return string.Equals(registered, CurrentExecutablePath, StringComparison.OrdinalIgnoreCase);
        }
    }

    /// <summary>Pulls the executable out of a Run command, quoted or not.</summary>
    public static string ExtractExecutable(string command)
    {
        if (string.IsNullOrWhiteSpace(command)) return string.Empty;

        command = command.Trim();
        if (command.StartsWith('"'))
        {
            int close = command.IndexOf('"', 1);
            return close > 1 ? command[1..close] : command.Trim('"');
        }

        // Unquoted: take everything up to the first argument separator that follows ".exe".
        int exe = command.IndexOf(".exe", StringComparison.OrdinalIgnoreCase);
        return exe > 0 ? command[..(exe + 4)] : command.Split(' ')[0];
    }
}
