using System.IO;
using System.Text.Json;
using DesktopSplitter.Models;

namespace DesktopSplitter.Services;

/// <summary>Reads/writes %APPDATA%\DesktopSplitter\settings.json (last applied UI state).</summary>
public static class SettingsStore
{
    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
    };

    public static string Directory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "DesktopSplitter");

    public static string FilePath => Path.Combine(Directory, "settings.json");

    public static AppSettings? Load()
    {
        try
        {
            if (!File.Exists(FilePath)) return null;
            string json = File.ReadAllText(FilePath);
            return JsonSerializer.Deserialize<AppSettings>(json, Options);
        }
        catch
        {
            // A corrupt settings file must never stop the app from starting.
            return null;
        }
    }

    public static void Save(AppSettings settings)
    {
        System.IO.Directory.CreateDirectory(Directory);
        File.WriteAllText(FilePath, JsonSerializer.Serialize(settings, Options));
    }
}
