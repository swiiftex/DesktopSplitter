using System.Runtime.InteropServices;

namespace DesktopSplitter.Interop;

/// <summary>Parsed fields from a monitor's EDID base block.</summary>
/// <param name="Manufacturer">Three-letter PnP vendor id from bytes 8-9 (big endian).</param>
/// <param name="ProductCode">uint16 LE at offset 10.</param>
/// <param name="SerialNumber">uint32 LE at offset 12.</param>
public sealed record EdidInfo(string Manufacturer, ushort ProductCode, uint SerialNumber)
{
    public override string ToString()
        => $"{Manufacturer} product=0x{ProductCode:X4} serial=0x{SerialNumber:X8}";
}

/// <summary>
/// Reads the raw EDID blob Windows caches under each monitor device's
/// "Device Parameters" registry key (value "EDID") and parses the identifying fields.
///
/// The DesktopSplitter driver stamps its synthesized EDIDs with manufacturer 'DSP'
/// (bytes 0x12 0x70 at offset 8), product code = monitor index, and
/// serial = 0xD5000001 + monitor index — which is what lets us map a segment to the
/// exact virtual monitor the driver created for it.
/// </summary>
public static class EdidReader
{
    /// <summary>GUID_DEVINTERFACE_MONITOR</summary>
    private static readonly Guid GUID_DEVINTERFACE_MONITOR =
        new("e6f07b5f-ee97-4a90-b076-33f57bf4eaa7");

    /// <summary>GUID_DEVCLASS_MONITOR</summary>
    private static readonly Guid GUID_DEVCLASS_MONITOR =
        new("4d36e96e-e325-11ce-bfc1-08002be10318");

    /// <summary>
    /// Snapshot of every present monitor's EDID, keyed by BOTH the device interface path
    /// (as returned by DISPLAYCONFIG_TARGET_DEVICE_NAME.monitorDevicePath) and the device
    /// instance id, lower-cased. Never throws.
    /// </summary>
    public static IReadOnlyDictionary<string, EdidInfo> BuildMap() => Sweep(ReadEdid);

    /// <summary>Reads something out of each present monitor's "Device Parameters" key.</summary>
    private delegate T? DevnodeReader<T>(IntPtr set, ref SP_DEVINFO_DATA devInfo) where T : class;

    private static IReadOnlyDictionary<string, T> Sweep<T>(DevnodeReader<T> reader) where T : class
    {
        var map = new Dictionary<string, T>(StringComparer.OrdinalIgnoreCase);
        try
        {
            AddFromInterfaceEnumeration(map, reader);
            AddFromClassEnumeration(map, reader);
        }
        catch (DllNotFoundException) { /* setupapi missing: fall back to heuristics */ }
        catch (EntryPointNotFoundException) { /* ditto */ }
        return map;
    }

    /// <summary>Looks up one monitor by its device interface path, using a fresh snapshot.</summary>
    public static bool TryRead(string monitorDevicePath, out EdidInfo? edid)
        => TryLookup(BuildMap(), monitorDevicePath, out edid);

    /// <summary>
    /// EDID-override markers on a monitor's devnode.
    /// <paramref name="HasOverride"/> is Windows' standard EDID_OVERRIDE; <paramref name="HasDeskSplitBackup"/>
    /// is EDID_OVERRIDE_DSBACKUP, which edidoverride.exe writes to stash the pre-DesktopSplitter
    /// state — its presence is what identifies an override as OURS rather than the OEM's or a
    /// hand-rolled one.
    /// </summary>
    public sealed record DevnodeEdidState(bool HasOverride, bool HasDeskSplitBackup);

    /// <summary>Override-marker state for every present monitor, keyed like <see cref="BuildMap"/>.</summary>
    public static IReadOnlyDictionary<string, DevnodeEdidState> BuildOverrideStateMap()
        => Sweep(ReadOverrideState);

    /// <summary>Looks up one monitor's override markers (interface path, then instance id).</summary>
    public static DevnodeEdidState? LookupOverrideState(
        IReadOnlyDictionary<string, DevnodeEdidState> map, string monitorDevicePath)
    {
        if (string.IsNullOrEmpty(monitorDevicePath) || map.Count == 0) return null;
        if (map.TryGetValue(monitorDevicePath, out DevnodeEdidState? direct)) return direct;

        string instanceId = DeviceInstanceIdFromInterfacePath(monitorDevicePath);
        return !string.IsNullOrEmpty(instanceId) && map.TryGetValue(instanceId, out DevnodeEdidState? viaInstance)
            ? viaInstance
            : null;
    }

    /// <summary>Looks up one monitor in an existing snapshot (interface path, then instance id).</summary>
    public static bool TryLookup(IReadOnlyDictionary<string, EdidInfo> map, string monitorDevicePath, out EdidInfo? edid)
    {
        edid = null;
        if (string.IsNullOrEmpty(monitorDevicePath) || map.Count == 0) return false;

        if (map.TryGetValue(monitorDevicePath, out EdidInfo? direct))
        {
            edid = direct;
            return true;
        }

        string instanceId = DeviceInstanceIdFromInterfacePath(monitorDevicePath);
        if (!string.IsNullOrEmpty(instanceId) && map.TryGetValue(instanceId, out EdidInfo? viaInstance))
        {
            edid = viaInstance;
            return true;
        }
        return false;
    }

    /// <summary>
    /// <c>\\?\DISPLAY#PHLC310#7&amp;224ecef0&amp;0&amp;UID260#{guid}</c>
    /// -&gt; <c>DISPLAY\PHLC310\7&amp;224ecef0&amp;0&amp;UID260</c>
    /// </summary>
    public static string DeviceInstanceIdFromInterfacePath(string interfacePath)
    {
        if (string.IsNullOrEmpty(interfacePath)) return string.Empty;

        string s = interfacePath;
        if (s.StartsWith(@"\\?\", StringComparison.Ordinal) || s.StartsWith(@"\\.\", StringComparison.Ordinal))
            s = s[4..];

        string[] parts = s.Split('#');
        // Last part is the interface class GUID in braces.
        int take = parts.Length;
        if (take > 0 && parts[take - 1].StartsWith('{')) take--;
        if (take <= 0) return string.Empty;

        return string.Join('\\', parts.Take(take));
    }

    /// <summary>Parses the 128-byte EDID base block. Returns null when the header is invalid.</summary>
    public static EdidInfo? Parse(byte[]? blob)
    {
        if (blob is null || blob.Length < 16) return null;

        // EDID 1.x header: 00 FF FF FF FF FF FF 00
        if (blob[0] != 0x00 || blob[7] != 0x00) return null;
        for (int i = 1; i <= 6; i++) if (blob[i] != 0xFF) return null;

        ushort vendor = (ushort)((blob[8] << 8) | blob[9]);   // big endian in EDID
        string manufacturer = DecodeVendor(vendor);
        ushort product = (ushort)(blob[10] | (blob[11] << 8));                       // LE
        uint serial = (uint)(blob[12] | (blob[13] << 8) | (blob[14] << 16) | (blob[15] << 24)); // LE

        return new EdidInfo(manufacturer, product, serial);
    }

    private static string DecodeVendor(ushort value)
    {
        int c1 = ((value >> 10) & 0x1F) + 'A' - 1;
        int c2 = ((value >> 5) & 0x1F) + 'A' - 1;
        int c3 = (value & 0x1F) + 'A' - 1;
        if (c1 < 'A' || c1 > 'Z' || c2 < 'A' || c2 > 'Z' || c3 < 'A' || c3 > 'Z') return string.Empty;
        return new string(new[] { (char)c1, (char)c2, (char)c3 });
    }

    // ------------------------------------------------------------------ enumeration

    private static void AddFromInterfaceEnumeration<T>(Dictionary<string, T> map, DevnodeReader<T> reader)
        where T : class
    {
        Guid interfaceGuid = GUID_DEVINTERFACE_MONITOR;
        IntPtr set = SetupDiGetClassDevs(ref interfaceGuid, IntPtr.Zero, IntPtr.Zero,
                                         DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return;

        try
        {
            for (uint i = 0; ; i++)
            {
                var ifData = default(SP_DEVICE_INTERFACE_DATA);
                ifData.cbSize = (uint)Marshal.SizeOf<SP_DEVICE_INTERFACE_DATA>();
                if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref interfaceGuid, i, ref ifData)) break;

                var devInfo = default(SP_DEVINFO_DATA);
                devInfo.cbSize = (uint)Marshal.SizeOf<SP_DEVINFO_DATA>();

                string path = GetInterfaceDetail(set, ref ifData, ref devInfo);
                T? value = reader(set, ref devInfo);
                if (value is null) continue;

                if (!string.IsNullOrEmpty(path)) map[path] = value;

                string instanceId = GetDeviceInstanceId(set, ref devInfo);
                if (!string.IsNullOrEmpty(instanceId)) map[instanceId] = value;
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    /// <summary>Fallback path: enumerate the Monitor device class and key by instance id only.</summary>
    private static void AddFromClassEnumeration<T>(Dictionary<string, T> map, DevnodeReader<T> reader)
        where T : class
    {
        Guid classGuid = GUID_DEVCLASS_MONITOR;
        IntPtr set = SetupDiGetClassDevs(ref classGuid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return;

        try
        {
            for (uint i = 0; ; i++)
            {
                var devInfo = default(SP_DEVINFO_DATA);
                devInfo.cbSize = (uint)Marshal.SizeOf<SP_DEVINFO_DATA>();
                if (!SetupDiEnumDeviceInfo(set, i, ref devInfo)) break;

                string instanceId = GetDeviceInstanceId(set, ref devInfo);
                if (string.IsNullOrEmpty(instanceId) || map.ContainsKey(instanceId)) continue;

                T? value = reader(set, ref devInfo);
                if (value is not null) map[instanceId] = value;
            }
        }
        finally
        {
            SetupDiDestroyDeviceInfoList(set);
        }
    }

    private static string GetInterfaceDetail(IntPtr set, ref SP_DEVICE_INTERFACE_DATA ifData, ref SP_DEVINFO_DATA devInfo)
    {
        SetupDiGetDeviceInterfaceDetail(set, ref ifData, IntPtr.Zero, 0, out uint required, IntPtr.Zero);
        if (required == 0) return string.Empty;

        IntPtr buffer = Marshal.AllocHGlobal((int)required);
        try
        {
            // SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize: 8 on 64-bit, 6 on 32-bit.
            Marshal.WriteInt32(buffer, IntPtr.Size == 8 ? 8 : 6);
            if (!SetupDiGetDeviceInterfaceDetail(set, ref ifData, buffer, required, out _, ref devInfo))
                return string.Empty;

            return Marshal.PtrToStringUni(buffer + 4) ?? string.Empty;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private static string GetDeviceInstanceId(IntPtr set, ref SP_DEVINFO_DATA devInfo)
    {
        var buffer = new char[512];
        return SetupDiGetDeviceInstanceId(set, ref devInfo, buffer, (uint)buffer.Length, out uint required)
            ? new string(buffer, 0, (int)Math.Max(0, Math.Min(required == 0 ? 0 : required - 1, buffer.Length)))
            : string.Empty;
    }

    private static EdidInfo? ReadEdid(IntPtr set, ref SP_DEVINFO_DATA devInfo)
    {
        IntPtr key = SetupDiOpenDevRegKey(set, ref devInfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE || key == IntPtr.Zero) return null;

        try
        {
            return Parse(ReadBinaryValue(key, "EDID"));
        }
        finally
        {
            RegCloseKey(key);
        }
    }

    /// <summary>
    /// Looks for the EDID override markers. Windows' own tooling stores EDID_OVERRIDE as a
    /// SUBKEY (containing a value "0"), while simpler tools write it as a value directly under
    /// Device Parameters — we accept either shape for both names so we stay compatible with
    /// however edidoverride.exe ends up writing them.
    /// </summary>
    private static DevnodeEdidState? ReadOverrideState(IntPtr set, ref SP_DEVINFO_DATA devInfo)
    {
        IntPtr key = SetupDiOpenDevRegKey(set, ref devInfo, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE || key == IntPtr.Zero) return null;

        try
        {
            return new DevnodeEdidState(
                HasValueOrSubkey(key, "EDID_OVERRIDE"),
                HasValueOrSubkey(key, "EDID_OVERRIDE_DSBACKUP"));
        }
        finally
        {
            RegCloseKey(key);
        }
    }

    private static bool HasValueOrSubkey(IntPtr key, string name)
    {
        uint size = 0;
        int rc = RegQueryValueEx(key, name, IntPtr.Zero, out _, IntPtr.Zero, ref size);
        if (rc == ERROR_SUCCESS || rc == ERROR_MORE_DATA) return true;

        if (RegOpenKeyEx(key, name, 0, KEY_READ, out IntPtr sub) == ERROR_SUCCESS)
        {
            RegCloseKey(sub);
            return true;
        }
        return false;
    }

    private static byte[]? ReadBinaryValue(IntPtr key, string valueName)
    {
        uint size = 0;
        int rc = RegQueryValueEx(key, valueName, IntPtr.Zero, out _, IntPtr.Zero, ref size);
        if (rc != ERROR_SUCCESS && rc != ERROR_MORE_DATA) return null;
        if (size == 0) return null;

        IntPtr data = Marshal.AllocHGlobal((int)size);
        try
        {
            rc = RegQueryValueEx(key, valueName, IntPtr.Zero, out _, data, ref size);
            if (rc != ERROR_SUCCESS) return null;

            var blob = new byte[size];
            Marshal.Copy(data, blob, 0, (int)size);
            return blob;
        }
        finally
        {
            Marshal.FreeHGlobal(data);
        }
    }

    // ------------------------------------------------------------------ P/Invoke

    private static readonly IntPtr INVALID_HANDLE_VALUE = new(-1);

    private const uint DIGCF_PRESENT = 0x00000002;
    private const uint DIGCF_DEVICEINTERFACE = 0x00000010;

    private const uint DICS_FLAG_GLOBAL = 0x00000001;
    private const uint DIREG_DEV = 0x00000001;
    private const uint KEY_READ = 0x00020019;

    private const int ERROR_SUCCESS = 0;
    private const int ERROR_MORE_DATA = 234;

    [StructLayout(LayoutKind.Sequential)]
    private struct SP_DEVICE_INTERFACE_DATA
    {
        public uint cbSize;
        public Guid InterfaceClassGuid;
        public uint Flags;
        public UIntPtr Reserved;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SP_DEVINFO_DATA
    {
        public uint cbSize;
        public Guid ClassGuid;
        public uint DevInst;
        public UIntPtr Reserved;
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "SetupDiGetClassDevsW")]
    private static extern IntPtr SetupDiGetClassDevs(
        ref Guid classGuid, IntPtr enumerator, IntPtr hwndParent, uint flags);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiEnumDeviceInterfaces(
        IntPtr deviceInfoSet, IntPtr deviceInfoData, ref Guid interfaceClassGuid,
        uint memberIndex, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "SetupDiGetDeviceInterfaceDetailW")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(
        IntPtr deviceInfoSet, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData,
        IntPtr deviceInterfaceDetailData, uint deviceInterfaceDetailDataSize,
        out uint requiredSize, IntPtr deviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "SetupDiGetDeviceInterfaceDetailW")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInterfaceDetail(
        IntPtr deviceInfoSet, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData,
        IntPtr deviceInterfaceDetailData, uint deviceInterfaceDetailDataSize,
        out uint requiredSize, ref SP_DEVINFO_DATA deviceInfoData);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiEnumDeviceInfo(
        IntPtr deviceInfoSet, uint memberIndex, ref SP_DEVINFO_DATA deviceInfoData);

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "SetupDiGetDeviceInstanceIdW")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiGetDeviceInstanceId(
        IntPtr deviceInfoSet, ref SP_DEVINFO_DATA deviceInfoData,
        [Out] char[] deviceInstanceId, uint deviceInstanceIdSize, out uint requiredSize);

    [DllImport("setupapi.dll", SetLastError = true)]
    private static extern IntPtr SetupDiOpenDevRegKey(
        IntPtr deviceInfoSet, ref SP_DEVINFO_DATA deviceInfoData,
        uint scope, uint hwProfile, uint keyType, uint samDesired);

    [DllImport("setupapi.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = false, EntryPoint = "RegQueryValueExW")]
    private static extern int RegQueryValueEx(
        IntPtr hKey, string lpValueName, IntPtr lpReserved,
        out uint lpType, IntPtr lpData, ref uint lpcbData);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = false, EntryPoint = "RegOpenKeyExW")]
    private static extern int RegOpenKeyEx(
        IntPtr hKey, string lpSubKey, uint ulOptions, uint samDesired, out IntPtr phkResult);

    [DllImport("advapi32.dll", SetLastError = false)]
    private static extern int RegCloseKey(IntPtr hKey);
}
