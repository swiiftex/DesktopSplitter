using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace DesktopSplitter.Interop;

/// <summary>Thrown when the DesktopSplitter driver device interface cannot be found.</summary>
public sealed class DriverNotFoundException : Exception
{
    public DriverNotFoundException(string message) : base(message) { }
}

/// <summary>Thrown when the driver is present but rejects/fails an IOCTL.</summary>
public sealed class DriverIoException : Exception
{
    public DriverIoException(string message) : base(message) { }
    public DriverIoException(string message, Exception inner) : base(message, inner) { }
}

// ---------------------------------------------------------------------------
// shared/DeskSplitProtocol.h — struct layout replicated EXACTLY (#pragma pack(4)).
// ---------------------------------------------------------------------------

[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct DESKSPLIT_MODE
{
    public uint Width;
    public uint Height;
    public uint RefreshMillihertz;
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct DESKSPLIT_MONITOR_CONFIG
{
    public uint ModeCount;
    public uint PreferredModeIndex;

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = DriverClient.DESKSPLIT_MAX_MODES)]
    public DESKSPLIT_MODE[] Modes;
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct DESKSPLIT_CONFIG
{
    public uint Version;
    public uint MonitorCount;

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = DriverClient.DESKSPLIT_MAX_MONITORS)]
    public DESKSPLIT_MONITOR_CONFIG[] Monitors;
}

[StructLayout(LayoutKind.Sequential, Pack = 4)]
public struct DESKSPLIT_STATUS
{
    public uint Version;
    public uint ActiveMonitorCount;
}

/// <summary>
/// Opens the driver's device interface and issues IOCTL_DESKSPLIT_SET_CONFIG /
/// IOCTL_DESKSPLIT_GET_STATUS.
/// </summary>
public static class DriverClient
{
    public const int DESKSPLIT_MAX_MONITORS = 4;
    public const int DESKSPLIT_MAX_MODES = 8;
    public const uint DESKSPLIT_PROTOCOL_VERSION = 1;

    /// <summary>{8E7A9B21-4C55-4C83-9B2E-7D3A1F60D2AD}</summary>
    public static readonly Guid GUID_DEVINTERFACE_DESKSPLIT =
        new(0x8e7a9b21, 0x4c55, 0x4c83, 0x9b, 0x2e, 0x7d, 0x3a, 0x1f, 0x60, 0xd2, 0xad);

    // CTL_CODE(DeviceType, Function, Method, Access)
    //   = (DeviceType << 16) | (Access << 14) | (Function << 2) | Method
    private const uint FILE_DEVICE_UNKNOWN = 0x00000022;
    private const uint METHOD_BUFFERED = 0;
    private const uint FILE_ANY_ACCESS = 0;

    private static uint CTL_CODE(uint deviceType, uint function, uint method, uint access)
        => (deviceType << 16) | (access << 14) | (function << 2) | method;

    /// <summary>0x00222400</summary>
    public static readonly uint IOCTL_DESKSPLIT_SET_CONFIG =
        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS);

    /// <summary>0x00222404</summary>
    public static readonly uint IOCTL_DESKSPLIT_GET_STATUS =
        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS);

    public const string DriverMissingMessage =
        "Driver device not found — install the driver first (installer\\install-driver.ps1).";

    /// <summary>Builds an all-zero-but-valid config with room for the fixed arrays.</summary>
    public static DESKSPLIT_CONFIG NewConfig()
    {
        var cfg = new DESKSPLIT_CONFIG
        {
            Version = DESKSPLIT_PROTOCOL_VERSION,
            MonitorCount = 0,
            Monitors = new DESKSPLIT_MONITOR_CONFIG[DESKSPLIT_MAX_MONITORS],
        };
        for (int i = 0; i < DESKSPLIT_MAX_MONITORS; i++)
        {
            cfg.Monitors[i] = new DESKSPLIT_MONITOR_CONFIG
            {
                ModeCount = 0,
                PreferredModeIndex = 0,
                Modes = new DESKSPLIT_MODE[DESKSPLIT_MAX_MODES],
            };
        }
        return cfg;
    }

    /// <summary>True when the driver's device interface is present.</summary>
    public static bool IsDriverPresent() => TryFindDevicePath(out _);

    /// <summary>Device interface path, or throws <see cref="DriverNotFoundException"/>.</summary>
    public static string GetDevicePath()
    {
        if (!TryFindDevicePath(out string path))
            throw new DriverNotFoundException(DriverMissingMessage);
        return path;
    }

    public static bool TryFindDevicePath(out string devicePath)
    {
        devicePath = string.Empty;
        Guid guid = GUID_DEVINTERFACE_DESKSPLIT;

        int cr = CM_Get_Device_Interface_List_Size(
            out uint len, ref guid, IntPtr.Zero, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != CR_SUCCESS || len < 2) return false;

        var buffer = new char[len];
        cr = CM_Get_Device_Interface_List(
            ref guid, IntPtr.Zero, buffer, len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != CR_SUCCESS) return false;

        foreach (string s in SplitMultiSz(buffer))
        {
            if (!string.IsNullOrWhiteSpace(s))
            {
                devicePath = s;
                return true;
            }
        }
        return false;
    }

    /// <summary>Sends IOCTL_DESKSPLIT_SET_CONFIG.</summary>
    public static void SetConfig(DESKSPLIT_CONFIG config)
    {
        if (config.Monitors is null || config.Monitors.Length != DESKSPLIT_MAX_MONITORS)
            throw new DriverIoException("DESKSPLIT_CONFIG.Monitors must be a fixed array of 4 entries.");
        if (config.MonitorCount > DESKSPLIT_MAX_MONITORS)
            throw new DriverIoException($"MonitorCount {config.MonitorCount} exceeds DESKSPLIT_MAX_MONITORS.");

        using SafeFileHandle handle = OpenDevice();

        int size = Marshal.SizeOf<DESKSPLIT_CONFIG>();
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(config, buffer, false);
            bool ok = DeviceIoControl(handle, IOCTL_DESKSPLIT_SET_CONFIG,
                                      buffer, (uint)size, IntPtr.Zero, 0,
                                      out _, IntPtr.Zero);
            if (!ok)
            {
                int err = Marshal.GetLastWin32Error();
                throw new DriverIoException(
                    $"IOCTL_DESKSPLIT_SET_CONFIG failed: {new Win32Exception(err).Message} (Win32 {err}).");
            }
        }
        finally
        {
            Marshal.DestroyStructure<DESKSPLIT_CONFIG>(buffer);
            Marshal.FreeHGlobal(buffer);
        }
    }

    /// <summary>Sends IOCTL_DESKSPLIT_GET_STATUS.</summary>
    public static DESKSPLIT_STATUS GetStatus()
    {
        using SafeFileHandle handle = OpenDevice();

        int size = Marshal.SizeOf<DESKSPLIT_STATUS>();
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try
        {
            bool ok = DeviceIoControl(handle, IOCTL_DESKSPLIT_GET_STATUS,
                                      IntPtr.Zero, 0, buffer, (uint)size,
                                      out uint returned, IntPtr.Zero);
            if (!ok)
            {
                int err = Marshal.GetLastWin32Error();
                throw new DriverIoException(
                    $"IOCTL_DESKSPLIT_GET_STATUS failed: {new Win32Exception(err).Message} (Win32 {err}).");
            }
            if (returned < size)
                throw new DriverIoException($"IOCTL_DESKSPLIT_GET_STATUS returned {returned} bytes, expected {size}.");

            return Marshal.PtrToStructure<DESKSPLIT_STATUS>(buffer);
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    /// <summary>Convenience: SET_CONFIG with MonitorCount = 0 (removes all virtual monitors).</summary>
    public static void ClearConfig()
    {
        DESKSPLIT_CONFIG cfg = NewConfig();
        cfg.MonitorCount = 0;
        SetConfig(cfg);
    }

    private static SafeFileHandle OpenDevice()
    {
        string path = GetDevicePath();
        SafeFileHandle handle = CreateFile(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            IntPtr.Zero,
            OPEN_EXISTING,
            0,
            IntPtr.Zero);

        if (handle.IsInvalid)
        {
            int err = Marshal.GetLastWin32Error();
            handle.Dispose();
            throw new DriverIoException(
                $"Could not open the driver device '{path}': {new Win32Exception(err).Message} (Win32 {err}).");
        }
        return handle;
    }

    private static IEnumerable<string> SplitMultiSz(char[] buffer)
    {
        int start = 0;
        for (int i = 0; i < buffer.Length; i++)
        {
            if (buffer[i] != '\0') continue;
            if (i > start) yield return new string(buffer, start, i - start);
            start = i + 1;
            if (start < buffer.Length && buffer[start] == '\0') yield break;   // terminating empty string
        }
    }

    // ------------------------------------------------------------------ P/Invoke

    private const int CR_SUCCESS = 0;
    private const uint CM_GET_DEVICE_INTERFACE_LIST_PRESENT = 0x00000001;

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, EntryPoint = "CM_Get_Device_Interface_List_SizeW")]
    private static extern int CM_Get_Device_Interface_List_Size(
        out uint pulLen, ref Guid interfaceClassGuid, IntPtr pDeviceID, uint ulFlags);

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, EntryPoint = "CM_Get_Device_Interface_ListW")]
    private static extern int CM_Get_Device_Interface_List(
        ref Guid interfaceClassGuid, IntPtr pDeviceID,
        [Out] char[] buffer, uint bufferLen, uint ulFlags);

    private const uint GENERIC_READ = 0x80000000;
    private const uint GENERIC_WRITE = 0x40000000;
    private const uint FILE_SHARE_READ = 0x00000001;
    private const uint FILE_SHARE_WRITE = 0x00000002;
    private const uint OPEN_EXISTING = 3;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateFileW")]
    private static extern SafeFileHandle CreateFile(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes,
        uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(
        SafeFileHandle hDevice, uint dwIoControlCode,
        IntPtr lpInBuffer, uint nInBufferSize,
        IntPtr lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);
}
