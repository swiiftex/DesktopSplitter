using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using DesktopSplitter.Interop;
using DesktopSplitter.Models;

// speccapture — ground-truth tooling for the "Remove display from desktop" flow.
//
//   speccapture capture              watch for SystemSettingsAdminFlows.exe and log its arguments
//   speccapture verify <targetId> <hash>   find which identity-hash recipe reproduces <hash>
//   speccapture candidates           dump the identity candidates and every recipe's hash
//
// Nothing here changes display state.

const string HelperName = "SystemSettingsAdminFlows";

string mode = args.Length > 0 ? args[0].ToLowerInvariant() : "capture";
string logPath = Path.Combine(AppContext.BaseDirectory, "speccapture.log");

switch (mode)
{
    case "capture": return Capture(logPath);
    case "verify": return Verify(args);
    case "candidates": return Candidates();
    default:
        Console.WriteLine("usage: speccapture [capture | verify <targetId> <hash> | candidates]");
        return 2;
}

// ---------------------------------------------------------------- capture
int Capture(string log)
{
    Console.WriteLine("=================================================================");
    Console.WriteLine(" DesktopSplitter — SystemSettingsAdminFlows capture");
    Console.WriteLine("=================================================================");
    Console.WriteLine();
    if (!IsElevated())
    {
        Console.WriteLine("  !! NOT RUNNING ELEVATED.");
        Console.WriteLine("     SystemSettingsAdminFlows.exe runs as administrator, and reading another");
        Console.WriteLine("     elevated process's command line requires elevation too.");
        Console.WriteLine("     Close this and re-run from an ADMINISTRATOR terminal.");
        Console.WriteLine();
        return 1;
    }

    Console.WriteLine("  Watching for SystemSettingsAdminFlows.exe ...");
    Console.WriteLine();
    Console.WriteLine("  NOW DO THIS:");
    Console.WriteLine("    1. Open  Settings > System > Display > Advanced display");
    Console.WriteLine("    2. Select the monitor you want to hide (the Philips 49M2C8900)");
    Console.WriteLine("    3. Turn ON  \"Remove display from desktop\"");
    Console.WriteLine("    4. Wait for the screen to settle, then turn it back OFF");
    Console.WriteLine();
    Console.WriteLine($"  Everything captured is appended to:  {log}");
    Console.WriteLine("  Press Ctrl+C when you are done.");
    Console.WriteLine();
    Console.WriteLine("-----------------------------------------------------------------");

    var seen = new HashSet<int>();
    int captured = 0;

    while (true)
    {
        foreach (Process p in Process.GetProcessesByName(HelperName))
        {
            using (p)
            {
                if (!seen.Add(p.Id)) continue;

                string? commandLine = TryReadCommandLine(p.Id);
                string stamp = DateTime.Now.ToString("HH:mm:ss.fff");
                string line = commandLine is null
                    ? $"[{stamp}] pid {p.Id}: (command line unavailable — is this elevated?)"
                    : $"[{stamp}] pid {p.Id}: {commandLine}";

                Console.WriteLine();
                Console.WriteLine("  >>> CAPTURED <<<");
                Console.WriteLine("  " + line);
                File.AppendAllText(log, line + Environment.NewLine);
                captured++;

                if (commandLine is not null) Explain(commandLine, log);
            }
        }

        // Tight poll: the helper is short-lived, and we must catch it while it still exists.
        Thread.Sleep(15);

        if (Console.KeyAvailable && Console.ReadKey(true).Key == ConsoleKey.Q) break;
        if (captured >= 40) break;
    }
    return 0;
}

void Explain(string commandLine, string log)
{
    string[] parts = SplitArgs(commandLine);
    int verb = Array.FindIndex(parts, a => a.Equals("SpecializeDisplay", StringComparison.OrdinalIgnoreCase));
    if (verb < 0 || parts.Length < verb + 6)
    {
        Console.WriteLine("      (not a SpecializeDisplay invocation)");
        return;
    }

    string adapterLow = parts[verb + 1], adapterHigh = parts[verb + 2];
    string targetId = parts[verb + 3], enable = parts[verb + 4], hash = parts[verb + 5];

    var sb = new StringBuilder();
    sb.AppendLine($"      adapterId.LowPart  = {adapterLow}");
    sb.AppendLine($"      adapterId.HighPart = {adapterHigh}");
    sb.AppendLine($"      targetId           = {targetId}");
    sb.AppendLine($"      enable             = {enable}");
    sb.AppendLine($"      monitor hash       = {hash}");
    sb.AppendLine();
    sb.AppendLine($"      -> now run:  speccapture verify {targetId} {hash}");
    Console.Write(sb);
    File.AppendAllText(log, sb.ToString());
}

// ---------------------------------------------------------------- verify
int Verify(string[] argv)
{
    if (argv.Length < 3 ||
        !uint.TryParse(argv[1], out uint targetId) ||
        !ulong.TryParse(argv[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out ulong captured))
    {
        Console.WriteLine("usage: speccapture verify <targetId> <hash>");
        return 2;
    }

    MonitorInfo? monitor = DisplayConfig.Enumerate().FirstOrDefault(m => m.TargetId == targetId);
    if (monitor is null)
    {
        Console.WriteLine($"No active monitor with targetId {targetId}. Active targets:");
        foreach (MonitorInfo m in DisplayConfig.Enumerate())
            Console.WriteLine($"  target={m.TargetId,-6} {m.DeviceName,-14} '{m.DisplayLabel}'");
        return 1;
    }

    Console.WriteLine(MonitorIdentityHash.Describe(monitor));
    Console.WriteLine($"captured hash: {captured}  (0x{captured:X16})");
    Console.WriteLine();
    Console.WriteLine("searching every identity x salt x split x byte-order combination...");
    Console.WriteLine();

    int tried = 0;
    var matches = new List<string>();
    foreach ((HashRecipe recipe, string identity) in MonitorIdentityHash.AllRecipes(monitor))
    {
        tried++;
        ulong? computed = MonitorIdentityHash.Compute(identity, recipe);
        if (computed is null) continue;

        bool hit = computed.Value == captured;
        if (hit)
        {
            matches.Add(recipe.Id);
            Console.WriteLine($"  *** MATCH ***  {recipe.Id}");
            Console.WriteLine($"                 identity = '{identity}'");
        }
    }

    Console.WriteLine();
    Console.WriteLine($"tried {tried} combinations.");
    if (matches.Count == 0)
    {
        Console.WriteLine("NO MATCH. The identity string is something we have not guessed.");
        Console.WriteLine("Send the captured line plus the candidate dump above back to the developer.");
        return 1;
    }

    Console.WriteLine();
    Console.WriteLine("RECIPE FOUND:");
    foreach (string m in matches) Console.WriteLine("  " + m);
    Console.WriteLine();
    Console.WriteLine("Record it so DesktopSplitter can use the automatic route, by setting");
    Console.WriteLine($"  \"specializationHashRecipe\": \"{matches[0]}\"");
    Console.WriteLine($"in  {Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "DesktopSplitter", "settings.json")}");
    return 0;
}

// ---------------------------------------------------------------- candidates
int Candidates()
{
    foreach (MonitorInfo m in DisplayConfig.Enumerate())
    {
        Console.WriteLine("=================================================================");
        Console.Write(MonitorIdentityHash.Describe(m));
        Console.WriteLine("  recipe hashes:");
        foreach ((HashRecipe recipe, string identity) in MonitorIdentityHash.AllRecipes(m))
        {
            ulong? h = MonitorIdentityHash.Compute(identity, recipe);
            if (h is not null) Console.WriteLine($"    {recipe.Id,-52} {h.Value}");
        }
        Console.WriteLine();
    }
    return 0;
}

// ---------------------------------------------------------------- helpers
static bool IsElevated()
{
    using var id = System.Security.Principal.WindowsIdentity.GetCurrent();
    return new System.Security.Principal.WindowsPrincipal(id)
        .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
}

static string[] SplitArgs(string commandLine)
{
    var list = new List<string>();
    var current = new StringBuilder();
    bool quoted = false;
    foreach (char c in commandLine)
    {
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted && char.IsWhiteSpace(c))
        {
            if (current.Length > 0) { list.Add(current.ToString()); current.Clear(); }
            continue;
        }
        current.Append(c);
    }
    if (current.Length > 0) list.Add(current.ToString());
    return list.ToArray();
}

/// <summary>
/// Reads another process's command line straight out of its PEB. Avoids taking a dependency on
/// the System.Management package just to run one WMI query.
/// </summary>
static string? TryReadCommandLine(int pid)
{
    const uint PROCESS_QUERY_INFORMATION = 0x0400;
    const uint PROCESS_VM_READ = 0x0010;

    IntPtr h = Native.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, pid);
    if (h == IntPtr.Zero) return null;

    try
    {
        var pbi = default(Native.PROCESS_BASIC_INFORMATION);
        int size = Marshal.SizeOf<Native.PROCESS_BASIC_INFORMATION>();
        if (Native.NtQueryInformationProcess(h, 0, ref pbi, size, out _) != 0) return null;
        if (pbi.PebBaseAddress == IntPtr.Zero) return null;

        // x64 PEB: ProcessParameters at +0x20.
        if (!ReadPtr(h, pbi.PebBaseAddress + 0x20, out IntPtr paramsPtr) || paramsPtr == IntPtr.Zero) return null;

        // x64 RTL_USER_PROCESS_PARAMETERS: CommandLine UNICODE_STRING at +0x70.
        var buffer = new byte[16];
        if (!Native.ReadProcessMemory(h, paramsPtr + 0x70, buffer, buffer.Length, out _)) return null;

        ushort length = BitConverter.ToUInt16(buffer, 0);
        IntPtr strPtr = (IntPtr)BitConverter.ToInt64(buffer, 8);
        if (length == 0 || strPtr == IntPtr.Zero || length > 32768) return null;

        var text = new byte[length];
        if (!Native.ReadProcessMemory(h, strPtr, text, text.Length, out _)) return null;

        return Encoding.Unicode.GetString(text);
    }
    catch
    {
        return null;
    }
    finally
    {
        Native.CloseHandle(h);
    }

    static bool ReadPtr(IntPtr h, IntPtr address, out IntPtr value)
    {
        var buf = new byte[8];
        value = IntPtr.Zero;
        if (!Native.ReadProcessMemory(h, address, buf, buf.Length, out _)) return false;
        value = (IntPtr)BitConverter.ToInt64(buf, 0);
        return true;
    }
}

internal static class Native
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct PROCESS_BASIC_INFORMATION
    {
        public IntPtr ExitStatus;
        public IntPtr PebBaseAddress;
        public IntPtr AffinityMask;
        public IntPtr BasePriority;
        public IntPtr UniqueProcessId;
        public IntPtr InheritedFromUniqueProcessId;
    }

    [DllImport("ntdll.dll")]
    internal static extern int NtQueryInformationProcess(
        IntPtr h, int cls, ref PROCESS_BASIC_INFORMATION info, int len, out int returned);

    [DllImport("kernel32.dll", SetLastError = true)]
    internal static extern IntPtr OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, int pid);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool ReadProcessMemory(
        IntPtr h, IntPtr baseAddress, [Out] byte[] buffer, int size, out IntPtr read);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static extern bool CloseHandle(IntPtr h);
}
