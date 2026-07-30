using System.Runtime.InteropServices;
using System.Text;

namespace DesktopSplitter.Interop;

/// <summary>
/// Reads another process's command line out of its PEB.
///
/// Used to observe the exact arguments Windows passes to SystemSettingsAdminFlows.exe when the
/// user flips "Remove display from desktop". Reading an ELEVATED process's memory requires the
/// reader to be elevated too, so this returns null rather than throwing when it cannot look.
/// (Deliberately avoids the System.Management NuGet package for one WMI query.)
/// </summary>
public static class ProcessCommandLine
{
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint PROCESS_VM_READ = 0x0010;

    /// <summary>x64 PEB: ProcessParameters pointer.</summary>
    private const int PebProcessParametersOffset = 0x20;

    /// <summary>x64 RTL_USER_PROCESS_PARAMETERS: CommandLine UNICODE_STRING.</summary>
    private const int ProcessParametersCommandLineOffset = 0x70;

    public static bool IsElevated()
    {
        try
        {
            using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
            return new System.Security.Principal.WindowsPrincipal(identity)
                .IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
        }
        catch
        {
            return false;
        }
    }

    /// <summary>The process's full command line, or null if it cannot be read.</summary>
    public static string? TryRead(int processId)
    {
        IntPtr handle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, false, processId);
        if (handle == IntPtr.Zero) return null;

        try
        {
            var basic = default(PROCESS_BASIC_INFORMATION);
            int size = Marshal.SizeOf<PROCESS_BASIC_INFORMATION>();
            if (NtQueryInformationProcess(handle, 0, ref basic, size, out _) != 0) return null;
            if (basic.PebBaseAddress == IntPtr.Zero) return null;

            if (!ReadPointer(handle, basic.PebBaseAddress + PebProcessParametersOffset, out IntPtr parameters))
                return null;
            if (parameters == IntPtr.Zero) return null;

            var descriptor = new byte[16];
            if (!ReadProcessMemory(handle, parameters + ProcessParametersCommandLineOffset,
                                   descriptor, descriptor.Length, out _))
            {
                return null;
            }

            ushort length = BitConverter.ToUInt16(descriptor, 0);
            var buffer = (IntPtr)BitConverter.ToInt64(descriptor, 8);
            if (length == 0 || buffer == IntPtr.Zero || length > 32768) return null;

            var text = new byte[length];
            if (!ReadProcessMemory(handle, buffer, text, text.Length, out _)) return null;

            return Encoding.Unicode.GetString(text);
        }
        catch
        {
            return null;
        }
        finally
        {
            CloseHandle(handle);
        }
    }

    /// <summary>Splits a command line into arguments, honouring double quotes.</summary>
    public static string[] SplitArguments(string commandLine)
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

    private static bool ReadPointer(IntPtr handle, IntPtr address, out IntPtr value)
    {
        var buffer = new byte[8];
        value = IntPtr.Zero;
        if (!ReadProcessMemory(handle, address, buffer, buffer.Length, out _)) return false;
        value = (IntPtr)BitConverter.ToInt64(buffer, 0);
        return true;
    }

    [StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct PROCESS_BASIC_INFORMATION
    {
        public IntPtr ExitStatus;
        public IntPtr PebBaseAddress;
        public IntPtr AffinityMask;
        public IntPtr BasePriority;
        public IntPtr UniqueProcessId;
        public IntPtr InheritedFromUniqueProcessId;
    }

    [DllImport("ntdll.dll")]
    private static extern int NtQueryInformationProcess(
        IntPtr handle, int infoClass, ref PROCESS_BASIC_INFORMATION info, int length, out int returned);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(
        uint access, [MarshalAs(UnmanagedType.Bool)] bool inheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ReadProcessMemory(
        IntPtr handle, IntPtr baseAddress, [Out] byte[] buffer, int size, out IntPtr read);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);
}
