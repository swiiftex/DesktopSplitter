using System.Runtime.InteropServices;
using System.Text;
using DesktopSplitter.Models;

namespace DesktopSplitter.Interop;

/// <summary>Where the GUID salt goes relative to the monitor identity string.</summary>
public enum SaltPosition
{
    Append,
    Prepend,
}

/// <summary>How the combined string is cut in two before hashing.</summary>
public enum SplitMode
{
    /// <summary>Both halves are the same masked length; a trailing odd WCHAR is dropped.</summary>
    EqualHalves,

    /// <summary>First half is the masked length, second half is everything that remains.</summary>
    FirstHalfThenRemainder,
}

/// <summary>Which half's hash becomes the low dword of the 64-bit result.</summary>
public enum HashByteOrder
{
    FirstIsLow,
    FirstIsHigh,
}

/// <summary>How the identity and the salt are fed to the hash function.</summary>
public enum HashMode
{
    /// <summary>Concatenate, then cut in two and hash each half.</summary>
    ConcatSplit,

    /// <summary>Hash the identity and the salt as two independent strings.</summary>
    SeparateStrings,

    /// <summary>Hash the whole concatenation once; the other dword mirrors it.</summary>
    WholeString,
}

/// <summary>Whether the split point is measured in bytes (UNICODE_STRING.Length) or characters.</summary>
public enum SplitUnit
{
    Bytes,
    Chars,
}

/// <summary>A named recipe for turning a monitor into the 64-bit identity hash.</summary>
public sealed record HashRecipe(
    string Identity,
    SaltPosition Salt,
    SplitMode Split,
    HashByteOrder Order,
    HashMode Mode = HashMode.ConcatSplit,
    bool CaseInsensitive = true,
    SplitUnit Unit = SplitUnit.Bytes)
{
    /// <summary>Stable id, e.g. "devpath|append|equal|firstlow|concat|ci|bytes". Safe to persist.</summary>
    public string Id => string.Join('|',
        Identity,
        Salt.ToString().ToLowerInvariant(),
        Split == SplitMode.EqualHalves ? "equal" : "remainder",
        Order == HashByteOrder.FirstIsLow ? "firstlow" : "firsthigh",
        Mode switch
        {
            HashMode.ConcatSplit => "concat",
            HashMode.SeparateStrings => "separate",
            _ => "whole",
        },
        CaseInsensitive ? "ci" : "cs",
        Unit == SplitUnit.Bytes ? "bytes" : "chars");

    public override string ToString() => Id;
}

public sealed record IdentityCandidate(string Name, string Value);

/// <summary>
/// Reproduces the 64-bit "monitor hash" that Windows passes as the 5th argument to
/// <c>SystemSettingsAdminFlows.exe SpecializeDisplay &lt;adapterLow&gt; &lt;adapterHigh&gt;
/// &lt;targetId&gt; &lt;enable&gt; &lt;hash&gt;</c>.
///
/// Reverse-engineered shape: take a stable monitor identity string, concatenate the specialization
/// GUID rendered by RtlStringFromGUID as a salt, split the result in two, run
/// RtlHashUnicodeString(CaseInSensitive = TRUE, algorithm = 0) over each half, and pack the two
/// 32-bit results into one 64-bit value.
///
/// Several details are unconfirmed — which identity string, salt position, split rule and dword
/// order — so this class enumerates every combination and the tooling identifies the right one by
/// matching a captured real-world hash. Nothing here guesses: until a recipe is proven, the app
/// does not use this path at all.
/// </summary>
public static class MonitorIdentityHash
{
    /// <summary>GUID_MONITOR_OVERRIDE_PSEUDO_SPECIALIZED — the salt.</summary>
    public static readonly Guid Salt = new("F196C02F-F86F-4F9A-AA15-E9CEBDFE3B96");

    private const uint HASH_STRING_ALGORITHM_DEFAULT = 0;
    private const int STATUS_SUCCESS = 0;

    /// <summary>The salt rendered exactly as RtlStringFromGUID renders it.</summary>
    public static string SaltString()
    {
        Guid salt = Salt;
        var us = default(UNICODE_STRING);
        if (RtlStringFromGUID(ref salt, ref us) == STATUS_SUCCESS && us.Buffer != IntPtr.Zero)
        {
            try
            {
                return Marshal.PtrToStringUni(us.Buffer, us.Length / 2) ?? FallbackSaltString();
            }
            finally
            {
                RtlFreeUnicodeString(ref us);
            }
        }
        return FallbackSaltString();
    }

    /// <summary>RtlStringFromGUID's format: uppercase hex inside braces.</summary>
    private static string FallbackSaltString() => Salt.ToString("B").ToUpperInvariant();

    /// <summary>
    /// Every plausible stable identity string for a monitor. The real one is unconfirmed, so the
    /// validator tries them all against a captured hash.
    /// </summary>
    public static IReadOnlyList<IdentityCandidate> CandidatesFor(MonitorInfo monitor)
    {
        string devPath = monitor.MonitorDevicePath ?? string.Empty;
        string instanceId = EdidReader.DeviceInstanceIdFromInterfacePath(devPath);

        var list = new List<IdentityCandidate>
        {
            new("devpath", devPath),
            new("devpath-upper", devPath.ToUpperInvariant()),
            new("devpath-lower", devPath.ToLowerInvariant()),
            new("instanceid", instanceId),
            new("instanceid-upper", instanceId.ToUpperInvariant()),
            new("instanceid-lower", instanceId.ToLowerInvariant()),
            new("gdiname", monitor.DeviceName ?? string.Empty),
            new("friendlyname", monitor.FriendlyName ?? string.Empty),
        };

        // The interface path minus its trailing interface-class GUID is another common form.
        int hash = devPath.LastIndexOf('#');
        if (hash > 0)
        {
            string trimmed = devPath[..hash];
            list.Add(new IdentityCandidate("devpath-noguid", trimmed));
            list.Add(new IdentityCandidate("devpath-noguid-upper", trimmed.ToUpperInvariant()));
        }

        if (devPath.Length > 0)
        {
            list.Add(new IdentityCandidate("devpath-trailingslash", devPath + "\\"));
            list.Add(new IdentityCandidate("devpath-backslashes", devPath.Replace('#', '\\')));
            list.Add(new IdentityCandidate("devpath-noprefix",
                devPath.StartsWith(@"\\?\", StringComparison.Ordinal) ? devPath[4..] : devPath));
        }

        if (!string.IsNullOrEmpty(instanceId))
            list.Add(new IdentityCandidate("instanceid-hashes", instanceId.Replace('\\', '#')));

        // EDID identity tuple: manufacturer + product code + serial.
        if (monitor.Edid is not null)
        {
            EdidInfo edid = monitor.Edid;
            list.Add(new IdentityCandidate("edid-tuple",
                $"{edid.Manufacturer}{edid.ProductCode:X4}{edid.SerialNumber:X8}"));
            list.Add(new IdentityCandidate("edid-tuple-dashes",
                $"{edid.Manufacturer}-{edid.ProductCode}-{edid.SerialNumber}"));
        }

        list.Add(new IdentityCandidate("target-id", monitor.TargetId.ToString()));
        list.Add(new IdentityCandidate("adapter-target",
            $"{monitor.AdapterIdHigh}-{monitor.AdapterIdLow}-{monitor.TargetId}"));

        return list
            .Where(c => !string.IsNullOrEmpty(c.Value))
            .GroupBy(c => c.Name)
            .Select(g => g.First())
            .ToList();
    }

    /// <summary>Every recipe over every identity candidate — the search space for validation.</summary>
    public static IEnumerable<(HashRecipe Recipe, string IdentityValue)> AllRecipes(MonitorInfo monitor)
    {
        foreach (IdentityCandidate candidate in CandidatesFor(monitor))
        {
            foreach (bool caseInsensitive in new[] { true, false })
            {
                foreach (HashByteOrder order in new[] { HashByteOrder.FirstIsLow, HashByteOrder.FirstIsHigh })
                {
                    // Identity and salt hashed as two independent strings.
                    foreach (SaltPosition salt in new[] { SaltPosition.Append, SaltPosition.Prepend })
                    {
                        yield return (new HashRecipe(candidate.Name, salt, SplitMode.EqualHalves, order,
                                                     HashMode.SeparateStrings, caseInsensitive), candidate.Value);
                    }

                    // One hash over the whole concatenation.
                    foreach (SaltPosition salt in new[] { SaltPosition.Append, SaltPosition.Prepend })
                    {
                        yield return (new HashRecipe(candidate.Name, salt, SplitMode.EqualHalves, order,
                                                     HashMode.WholeString, caseInsensitive), candidate.Value);
                    }

                    // Concatenate then split.
                    foreach (SaltPosition salt in new[] { SaltPosition.Append, SaltPosition.Prepend })
                    foreach (SplitMode split in new[] { SplitMode.EqualHalves, SplitMode.FirstHalfThenRemainder })
                    foreach (SplitUnit unit in new[] { SplitUnit.Bytes, SplitUnit.Chars })
                    {
                        yield return (new HashRecipe(candidate.Name, salt, split, order,
                                                     HashMode.ConcatSplit, caseInsensitive, unit), candidate.Value);
                    }
                }
            }
        }
    }

    /// <summary>Computes the hash for a monitor using a named recipe, or null if unavailable.</summary>
    public static ulong? Compute(MonitorInfo monitor, string recipeId)
    {
        foreach ((HashRecipe recipe, string identity) in AllRecipes(monitor))
        {
            if (recipe.Id == recipeId) return Compute(identity, recipe);
        }
        return null;
    }

    /// <summary>Computes the hash for an explicit identity string and recipe.</summary>
    public static ulong? Compute(string identity, HashRecipe recipe)
    {
        if (string.IsNullOrEmpty(identity)) return null;

        string salt = SaltString();
        bool ci = recipe.CaseInsensitive;

        if (recipe.Mode == HashMode.SeparateStrings)
        {
            string a = recipe.Salt == SaltPosition.Append ? identity : salt;
            string b = recipe.Salt == SaltPosition.Append ? salt : identity;
            if (!TryHashRange(a, 0, a.Length * 2, ci, out uint ha)) return null;
            if (!TryHashRange(b, 0, b.Length * 2, ci, out uint hb)) return null;
            return Pack(ha, hb, recipe.Order);
        }

        string combined = recipe.Salt == SaltPosition.Append ? identity + salt : salt + identity;
        if (combined.Length < 2) return null;
        int totalBytes = combined.Length * 2;

        if (recipe.Mode == HashMode.WholeString)
        {
            if (!TryHashRange(combined, 0, totalBytes, ci, out uint whole)) return null;
            return Pack(whole, whole, recipe.Order);
        }

        int firstBytes = recipe.Unit == SplitUnit.Bytes
            ? (totalBytes / 2) & 0x7FFE
            : (combined.Length / 2) * 2;
        if (firstBytes <= 0 || firstBytes >= totalBytes) return null;

        int secondBytes = recipe.Split == SplitMode.EqualHalves ? firstBytes : totalBytes - firstBytes;
        if (secondBytes <= 0 || firstBytes + secondBytes > totalBytes) return null;

        if (!TryHashRange(combined, 0, firstBytes, ci, out uint first)) return null;
        if (!TryHashRange(combined, firstBytes, secondBytes, ci, out uint second)) return null;

        return Pack(first, second, recipe.Order);
    }

    private static ulong Pack(uint first, uint second, HashByteOrder order)
        => order == HashByteOrder.FirstIsLow
            ? ((ulong)second << 32) | first
            : ((ulong)first << 32) | second;

    /// <summary>Runs RtlHashUnicodeString over a byte range of a string.</summary>
    private static bool TryHashRange(string value, int byteOffset, int byteLength, bool caseInsensitive, out uint hash)
    {
        hash = 0;
        if (byteLength <= 0) return false;

        GCHandle pin = GCHandle.Alloc(value, GCHandleType.Pinned);
        try
        {
            IntPtr basePtr = pin.AddrOfPinnedObject();
            var us = new UNICODE_STRING
            {
                Length = (ushort)byteLength,
                MaximumLength = (ushort)byteLength,
                Buffer = basePtr + byteOffset,
            };
            return RtlHashUnicodeString(ref us, caseInsensitive, HASH_STRING_ALGORITHM_DEFAULT, out hash)
                   == STATUS_SUCCESS;
        }
        finally
        {
            pin.Free();
        }
    }

    /// <summary>Human-readable dump of every candidate/recipe, for the validation tool.</summary>
    public static string Describe(MonitorInfo monitor)
    {
        var sb = new StringBuilder();
        sb.AppendLine($"monitor : {monitor.DisplayLabel} ({monitor.DeviceName})");
        sb.AppendLine($"adapter : low={monitor.AdapterIdLow} high={monitor.AdapterIdHigh}  target={monitor.TargetId}");
        sb.AppendLine($"salt    : {SaltString()}");
        sb.AppendLine("identity candidates:");
        foreach (IdentityCandidate c in CandidatesFor(monitor))
            sb.AppendLine($"    {c.Name,-22} '{c.Value}'");
        return sb.ToString();
    }

    // ------------------------------------------------------------------ P/Invoke

    [StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct UNICODE_STRING
    {
        public ushort Length;
        public ushort MaximumLength;
        public IntPtr Buffer;
    }

    [DllImport("ntdll.dll")]
    private static extern int RtlHashUnicodeString(
        ref UNICODE_STRING String,
        [MarshalAs(UnmanagedType.U1)] bool CaseInSensitive,
        uint HashAlgorithm,
        out uint HashValue);

    [DllImport("ntdll.dll")]
    private static extern int RtlStringFromGUID(ref Guid Guid, ref UNICODE_STRING GuidString);

    [DllImport("ntdll.dll")]
    private static extern void RtlFreeUnicodeString(ref UNICODE_STRING UnicodeString);
}

