// edidoverride.exe - inspect and patch a monitor's EDID override so Windows
// will treat the display as a "specialized" (non-desktop) monitor.
//
// READ docs\HIDING.md AND README.md IN THIS DIRECTORY BEFORE USING `apply`.
// A bad EDID override can leave a display unusable; recovery is documented in
// README.md and requires deleting a registry key (possible from Safe Mode).

// WIN32_LEAN_AND_MEAN / NOMINMAX come from the project's preprocessor settings.
#include <Windows.h>

#include "EdidPatch.h"
#include "Guard.h"

#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

namespace {

using edid::Bytes;

const wchar_t* const kEnumRoot = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY";
const wchar_t* const kOverrideKey = L"EDID_OVERRIDE";
const wchar_t* const kBackupKey = L"EDID_OVERRIDE_DSBACKUP";
const wchar_t* const kBackupMarker = L"DesktopSplitterBackup";

struct Monitor {
    std::wstring hardwareId;    // PHLC310
    std::wstring instanceId;    // 7&224ecef0&0&UID260
    std::wstring regPath;       // SYSTEM\...\Enum\DISPLAY\PHLC310\7&...
    std::wstring gdiName;       // \\.\DISPLAY1 (empty when not on the desktop)
    std::wstring friendlyName;
    Bytes        edid;
    bool         hasOverride = false;
    bool         hasBackup = false;
};

void Out(const wchar_t* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    ::vfwprintf(stdout, fmt, a);
    va_end(a);
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION el = {};
    DWORD cb = sizeof(el);
    const BOOL ok = ::GetTokenInformation(token, TokenElevation, &el, sizeof(el), &cb);
    ::CloseHandle(token);
    return ok && el.TokenIsElevated != 0;
}

// Devnode ids are ASCII; narrow explicitly so there is no implicit conversion.
std::string NarrowAscii(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) {
        s.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
    }
    return s;
}

// Stable per-monitor seed for the VSDB Container ID.
std::string ContainerSeed(const std::wstring& hardwareId,
                          const std::wstring& instanceId) {
    return NarrowAscii(hardwareId) + "\\" + NarrowAscii(instanceId);
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

bool ReadBinaryValue(HKEY key, const wchar_t* name, Bytes& out) {
    DWORD type = 0;
    DWORD cb = 0;
    if (::RegQueryValueExW(key, name, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS) {
        return false;
    }
    if (type != REG_BINARY || cb == 0) return false;
    out.resize(cb);
    return ::RegQueryValueExW(key, name, nullptr, &type, out.data(), &cb) == ERROR_SUCCESS;
}

bool OpenDeviceParameters(const Monitor& m, REGSAM access, HKEY& out) {
    const std::wstring path = m.regPath + L"\\Device Parameters";
    return ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, access, &out) ==
           ERROR_SUCCESS;
}

bool SubkeyExists(const Monitor& m, const wchar_t* sub) {
    const std::wstring path = m.regPath + L"\\Device Parameters\\" + sub;
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &k) !=
        ERROR_SUCCESS) {
        return false;
    }
    ::RegCloseKey(k);
    return true;
}

// Reassembles blocks "0","1",... from an EDID_OVERRIDE-style key.
bool ReadOverrideBlocks(const Monitor& m, const wchar_t* sub, Bytes& out) {
    const std::wstring path = m.regPath + L"\\Device Parameters\\" + sub;
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &k) !=
        ERROR_SUCCESS) {
        return false;
    }
    out.clear();
    for (int i = 0;; ++i) {
        wchar_t name[16];
        ::_snwprintf_s(name, _countof(name), _TRUNCATE, L"%d", i);
        Bytes block;
        if (!ReadBinaryValue(k, name, block)) break;
        out.insert(out.end(), block.begin(), block.end());
    }
    ::RegCloseKey(k);
    return !out.empty();
}

std::wstring RegStringValue(HKEY key, const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    if (::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(buf),
                           &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        return buf;
    }
    return std::wstring();
}

// interface path "\\?\DISPLAY#PHLC310#7&224...#{guid}" -> hardware + instance id
bool SplitInterfacePath(const std::wstring& iface, std::wstring& hw, std::wstring& inst) {
    const size_t p = Lower(iface).find(L"display#");
    if (p == std::wstring::npos) return false;
    std::wstring rest = iface.substr(p + 8);
    const size_t h1 = rest.find(L'#');
    if (h1 == std::wstring::npos) return false;
    hw = rest.substr(0, h1);
    rest = rest.substr(h1 + 1);
    const size_t h2 = rest.find(L'#');
    inst = (h2 == std::wstring::npos) ? rest : rest.substr(0, h2);
    return !hw.empty() && !inst.empty();
}

// GDI device name for each attached monitor, keyed by "hardwareId\instanceId".
void CollectGdiNames(std::vector<std::pair<std::wstring, std::wstring>>& out) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    for (DWORD ai = 0; ::EnumDisplayDevicesW(nullptr, ai, &adapter, 0); ++ai) {
        if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) continue;
        DISPLAY_DEVICEW mon = {};
        mon.cb = sizeof(mon);
        if (!::EnumDisplayDevicesW(adapter.DeviceName, 0, &mon,
                                   EDD_GET_DEVICE_INTERFACE_NAME)) {
            continue;
        }
        std::wstring hw, inst;
        if (SplitInterfacePath(mon.DeviceID, hw, inst)) {
            out.emplace_back(Lower(hw + L"\\" + inst), adapter.DeviceName);
        }
    }
}

std::vector<Monitor> EnumerateMonitors() {
    std::vector<Monitor> out;
    std::vector<std::pair<std::wstring, std::wstring>> gdi;
    CollectGdiNames(gdi);

    HKEY root = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kEnumRoot, 0, KEY_READ, &root) !=
        ERROR_SUCCESS) {
        return out;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t hw[256];
        DWORD hwLen = _countof(hw);
        if (::RegEnumKeyExW(root, i, hw, &hwLen, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS) {
            break;
        }
        HKEY hwKey = nullptr;
        if (::RegOpenKeyExW(root, hw, 0, KEY_READ, &hwKey) != ERROR_SUCCESS) continue;
        for (DWORD j = 0;; ++j) {
            wchar_t inst[256];
            DWORD instLen = _countof(inst);
            if (::RegEnumKeyExW(hwKey, j, inst, &instLen, nullptr, nullptr, nullptr,
                                nullptr) != ERROR_SUCCESS) {
                break;
            }
            Monitor m;
            m.hardwareId = hw;
            m.instanceId = inst;
            m.regPath = std::wstring(kEnumRoot) + L"\\" + hw + L"\\" + inst;

            HKEY instKey = nullptr;
            if (::RegOpenKeyExW(hwKey, inst, 0, KEY_READ, &instKey) == ERROR_SUCCESS) {
                m.friendlyName = RegStringValue(instKey, L"DeviceDesc");
                const size_t semi = m.friendlyName.rfind(L';');
                if (semi != std::wstring::npos) m.friendlyName = m.friendlyName.substr(semi + 1);
                ::RegCloseKey(instKey);
            }
            HKEY dp = nullptr;
            if (OpenDeviceParameters(m, KEY_READ, dp)) {
                ReadBinaryValue(dp, L"EDID", m.edid);
                ::RegCloseKey(dp);
            }
            m.hasOverride = SubkeyExists(m, kOverrideKey);
            m.hasBackup = SubkeyExists(m, kBackupKey);

            const std::wstring gkey = Lower(m.hardwareId + L"\\" + m.instanceId);
            for (const auto& g : gdi) {
                if (g.first == gkey) { m.gdiName = g.second; break; }
            }
            out.push_back(std::move(m));
        }
        ::RegCloseKey(hwKey);
    }
    ::RegCloseKey(root);
    return out;
}

const Monitor* ResolveTarget(const std::vector<Monitor>& mons, const std::wstring& spec) {
    const std::wstring want = Lower(spec);
    for (const Monitor& m : mons) {                       // exact GDI name
        if (!m.gdiName.empty() && Lower(m.gdiName) == want) return &m;
    }
    for (const Monitor& m : mons) {                       // devnode substring
        const std::wstring key = Lower(m.hardwareId + L"\\" + m.instanceId);
        if (key.find(want) != std::wstring::npos) return &m;
    }
    return nullptr;
}

void PrintEdidSummary(const Bytes& e, const wchar_t* label) {
    Out(L"  %s: %zu bytes (%zu block(s))\n", label, e.size(), edid::BlockCount(e));
    if (e.empty()) return;
    Out(L"    header valid   : %s\n", edid::HasValidHeader(e) ? L"yes" : L"NO");
    Out(L"    EDID version   : %u.%u\n", e.size() > 19 ? e[18] : 0,
        e.size() > 19 ? e[19] : 0);
    Out(L"    ext count byte : %u  (blocks present: %zu)\n",
        edid::DeclaredExtensionCount(e), edid::BlockCount(e));
    for (size_t b = 0; b < edid::BlockCount(e); ++b) {
        Out(L"    block %zu: tag 0x%02X  checksum %s\n", b,
            b == 0 ? 0 : e[b * edid::kBlockSize],
            edid::BlockChecksumOk(e.data() + b * edid::kBlockSize) ? L"OK" : L"BAD");
    }
    const edid::CtaBlockInfo cta = edid::FindFirstCtaBlock(e);
    if (cta.found) {
        Out(L"    first CTA block: index %zu, rev %u, dtdStart %u, %zu data-block "
            L"byte(s), %zu DTD(s), %zu free byte(s)\n",
            cta.blockIndex, cta.revision, cta.dtdStart, cta.dataBlockBytes,
            cta.dtdCount, cta.freeBytes);
    } else {
        Out(L"    first CTA block: none\n");
    }
    const edid::VsdbLocation v = edid::FindMicrosoftVsdb(e);
    if (v.found) {
        Out(L"    Microsoft VSDB : PRESENT at offset %zu, version 0x%02X, flags 0x%02X, "
            L"desktop-usage %s => %s\n",
            v.offset, v.version, v.flags, v.desktopUsage ? L"SET" : L"clear",
            (!v.desktopUsage || v.version == 1 || v.version == 2)
                ? L"NON-DESKTOP (specialized)"
                : L"desktop");
    } else {
        Out(L"    Microsoft VSDB : absent  => Windows treats this as a desktop monitor\n");
    }
}

void HexDump(const Bytes& e, const wchar_t* label) {
    Out(L"\n  %s (%zu bytes)\n", label, e.size());
    for (size_t i = 0; i < e.size(); i += 16) {
        Out(L"    %04zX  ", i);
        for (size_t j = 0; j < 16 && i + j < e.size(); ++j) Out(L"%02X ", e[i + j]);
        Out(L"\n");
    }
}

// ---------------------------------------------------------------------------
// Argument parsing (a pure function so `selftest` can exercise it)
// ---------------------------------------------------------------------------

struct ParsedArgs {
    std::wstring cmd;
    std::wstring target;
    bool         yes = false;
    bool         hex = false;
    bool         dryRun = false;
    uint8_t      useCase = 0x02;
    int          timeoutSeconds = 10;
    bool         ok = true;
    bool         wantsHelp = false;
    std::wstring error;
};

ParsedArgs ParseArgs(int argc, const wchar_t* const* argv) {
    ParsedArgs a;
    if (argc < 2) {
        a.ok = false;
        a.wantsHelp = true;
        a.error = L"no command given";
        return a;
    }
    a.cmd = argv[1];
    if (a.cmd == L"--help" || a.cmd == L"-h" || a.cmd == L"help") {
        a.wantsHelp = true;
        return a;
    }
    for (int i = 2; i < argc; ++i) {
        const std::wstring t = argv[i];
        if (t == L"--yes") {
            a.yes = true;
        } else if (t == L"--hex") {
            a.hex = true;
        } else if (t == L"--dry-run") {
            a.dryRun = true;
        } else if (t == L"--use-case") {
            if (i + 1 >= argc) {
                a.ok = false;
                a.error = L"--use-case needs a value";
                return a;
            }
            a.useCase = static_cast<uint8_t>(::_wtoi(argv[++i]) & 0x1F);
        } else if (t == L"--timeout-seconds") {
            if (i + 1 >= argc) {
                a.ok = false;
                a.error = L"--timeout-seconds needs a value";
                return a;
            }
            const int v = ::_wtoi(argv[++i]);
            if (v < 1 || v > 600) {
                a.ok = false;
                a.error = L"--timeout-seconds must be between 1 and 600";
                return a;
            }
            a.timeoutSeconds = v;
        } else if (!t.empty() && t[0] != L'-' && a.target.empty()) {
            a.target = t;
        } else {
            a.ok = false;
            a.error = L"unknown argument '" + t + L"'";
            return a;
        }
    }
    return a;
}

// --- commands --------------------------------------------------------------

int CmdList(const std::vector<Monitor>& mons) {
    Out(L"Monitors known to Windows (HKLM\\%s):\n\n", kEnumRoot);
    for (const Monitor& m : mons) {
        Out(L"  %s\\%s\n", m.hardwareId.c_str(), m.instanceId.c_str());
        Out(L"      desc     : %s\n", m.friendlyName.c_str());
        Out(L"      GDI name : %s\n",
            m.gdiName.empty() ? L"<not attached to the desktop>" : m.gdiName.c_str());
        Out(L"      EDID     : %zu bytes\n", m.edid.size());
        Out(L"      override : %s%s\n", m.hasOverride ? L"PRESENT" : L"none",
            m.hasBackup ? L"   (DesktopSplitter backup present)" : L"");
        if (!m.edid.empty()) {
            const edid::VsdbLocation v = edid::FindMicrosoftVsdb(m.edid);
            Out(L"      MS VSDB  : %s\n", v.found ? L"present" : L"absent");
        }
        Out(L"\n");
    }
    return 0;
}

int CmdDump(const Monitor& m, bool hex) {
    Out(L"Target: %s\\%s   %s\n", m.hardwareId.c_str(), m.instanceId.c_str(),
        m.gdiName.empty() ? L"" : m.gdiName.c_str());
    Out(L"Registry: HKLM\\%s\n\n", m.regPath.c_str());

    Out(L"CURRENT effective EDID (Device Parameters\\EDID)\n");
    PrintEdidSummary(m.edid, L"current");

    Bytes ov;
    if (ReadOverrideBlocks(m, kOverrideKey, ov)) {
        Out(L"\nEXISTING override (Device Parameters\\%s)\n", kOverrideKey);
        PrintEdidSummary(ov, L"override");
        Out(L"    matches current: %s\n", ov == m.edid ? L"yes" : L"NO");
    } else {
        Out(L"\nEXISTING override: none\n");
    }
    Bytes bk;
    if (ReadOverrideBlocks(m, kBackupKey, bk)) {
        Out(L"\nDesktopSplitter backup (Device Parameters\\%s)\n", kBackupKey);
        PrintEdidSummary(bk, L"backup");
    }
    if (hex) {
        HexDump(m.edid, L"current EDID");
        if (!ov.empty()) HexDump(ov, L"override EDID");
    }
    return 0;
}

int CmdVerify(const Monitor& m) {
    int rc = 0;
    Out(L"Verifying %s\\%s\n\n", m.hardwareId.c_str(), m.instanceId.c_str());
    if (m.edid.empty()) {
        Out(L"  FAIL: no EDID recorded for this monitor\n");
        return 1;
    }
    Out(L"  current EDID checksums: %s\n",
        edid::AllChecksumsOk(m.edid) ? L"all OK" : L"BAD");
    if (!edid::AllChecksumsOk(m.edid)) rc = 1;

    Bytes ov;
    if (ReadOverrideBlocks(m, kOverrideKey, ov)) {
        Out(L"  override present, %zu bytes, checksums: %s\n", ov.size(),
            edid::AllChecksumsOk(ov) ? L"all OK" : L"BAD");
        if (!edid::AllChecksumsOk(ov)) rc = 1;
        if (edid::DeclaredExtensionCount(ov) + 1u != edid::BlockCount(ov)) {
            Out(L"  FAIL: override extension count (%u) does not match blocks (%zu)\n",
                edid::DeclaredExtensionCount(ov), edid::BlockCount(ov));
            rc = 1;
        }
    } else {
        Out(L"  override: none\n");
    }
    const edid::VsdbLocation v = edid::FindMicrosoftVsdb(m.edid);
    Out(L"  Microsoft VSDB: %s\n", v.found ? L"present" : L"absent");
    if (v.found) {
        Out(L"    version 0x%02X, desktop-usage bit %s\n", v.version,
            v.desktopUsage ? L"SET (still a desktop monitor!)" : L"clear (specialized)");
        if (v.desktopUsage) rc = 1;
    }
    Out(L"\n  result: %s\n", rc == 0 ? L"PASS" : L"PROBLEMS FOUND");
    return rc;
}

int CmdSelfTest(const Monitor& m, uint8_t useCase) {
    Out(L"Self-test against the REAL EDID of %s\\%s (nothing is written)\n\n",
        m.hardwareId.c_str(), m.instanceId.c_str());
    if (m.edid.empty()) {
        Out(L"  FAIL: no EDID to test with\n");
        return 1;
    }

    int failures = 0;
    auto check = [&](bool ok, const wchar_t* what) {
        Out(L"  %s  %s\n", ok ? L"pass" : L"FAIL", what);
        if (!ok) ++failures;
    };

    check(edid::HasValidHeader(m.edid), L"original EDID has a valid header");
    check(edid::AllChecksumsOk(m.edid), L"original EDID checksums are all valid");
    check(edid::DeclaredExtensionCount(m.edid) + 1u == edid::BlockCount(m.edid),
          L"original extension count matches the blocks present");
    check(!edid::FindMicrosoftVsdb(m.edid).found,
          L"original EDID has no Microsoft VSDB");

    const edid::CtaBlockInfo cta = edid::FindFirstCtaBlock(m.edid);
    check(cta.found, L"original EDID contains a CTA-861 extension block");
    std::vector<Bytes> db, dtd;
    if (cta.found) {
        check(edid::ExtractCtaDataBlocks(m.edid, cta, db),
              L"original CTA data block collection parses cleanly");
        edid::ExtractCtaDtds(m.edid, cta, dtd);
        Out(L"        (%zu data block(s), %zu DTD(s), %zu free byte(s) in the CTA block)\n",
            db.size(), dtd.size(), cta.freeBytes);
    }

    edid::VsdbOptions opts;
    opts.primaryUseCase = useCase;
    opts.containerId =
        edid::DeriveContainerId(ContainerSeed(m.hardwareId, m.instanceId));

    const Bytes vsdb = edid::BuildMicrosoftVsdb(opts);
    check(vsdb.size() == edid::kVsdbSize, L"built VSDB is 22 bytes");
    check(vsdb[0] == 0x75, L"VSDB header byte is 0x75 (tag 3, payload length 21)");
    check(vsdb[1] == 0x5C && vsdb[2] == 0x12 && vsdb[3] == 0xCA,
          L"VSDB OUI bytes are 5C 12 CA in sequential order");
    check(vsdb[4] == 0x03, L"VSDB version byte is 0x03");
    check((vsdb[5] & 0x40) == 0, L"VSDB desktop-usage bit (bit 6) is CLEAR");

    const edid::PatchResult pr = edid::AddMicrosoftVsdb(m.edid, opts);
    if (!pr.ok) {
        Out(L"  FAIL  patch failed: %S\n", pr.error.c_str());
        return 1;
    }
    Out(L"  pass  patch succeeded using strategy: %S\n", edid::StrategyName(pr.strategy));
    Out(L"        %zu bytes -> %zu bytes\n", m.edid.size(), pr.output.size());

    const edid::RoundTripReport rep =
        edid::VerifyRoundTrip(m.edid, pr.output, pr.strategy);
    if (!rep.ok) {
        Out(L"  FAIL  round-trip: %S\n", rep.error.c_str());
        ++failures;
    } else {
        Out(L"  pass  round-trip: only the VSDB, bookkeeping bytes and checksums "
            L"changed\n");
        Out(L"        %zu byte(s) differ in the common prefix; %zu data block(s) and "
            L"%zu DTD(s) preserved verbatim\n",
            rep.differingBytes, rep.preservedDataBlocks, rep.preservedDtds);
    }

    const edid::VsdbLocation found = edid::FindMicrosoftVsdb(pr.output);
    check(found.found, L"patched EDID's VSDB is discoverable by the parser");
    check(found.version == 0x03, L"patched VSDB parses back as version 3");
    check(!found.desktopUsage, L"patched VSDB parses back as non-desktop");
    check(edid::AllChecksumsOk(pr.output), L"every block checksum in the patched EDID is valid");

    // Idempotence: patching twice must be refused, not duplicated.
    const edid::PatchResult again = edid::AddMicrosoftVsdb(pr.output, opts);
    check(!again.ok, L"re-patching an already-patched EDID is refused");

    // Determinism: the same input must produce byte-identical output.
    const edid::PatchResult repeat = edid::AddMicrosoftVsdb(m.edid, opts);
    check(repeat.ok && repeat.output == pr.output,
          L"patching is deterministic (identical bytes on a second run)");

    // --- guard event security ------------------------------------------
    Out(L"\n  -- guard event security --\n");
    {
        guard::GuardEvents ev;
        const bool created = ev.Create();
        check(created, L"guard events can be created");
        if (created) {
            Out(L"        (namespace: %s)\n",
                ev.usedGlobalNamespace ? L"Global" : L"Local - unelevated fallback");
            const HANDLE handles[3] = { ev.armed, ev.keep, ev.revert };
            const wchar_t* names[3] = { L"Armed", L"Keep", L"Revert" };
            for (int i = 0; i < 3; ++i) {
                std::wstring detail;
                const bool ok = guard::VerifyEventDacl(handles[i], detail);
                wchar_t what[160];
                ::_snwprintf_s(what, _countof(what), _TRUNCATE,
                               L"%s event DACL grants the unelevated user "
                               L"EVENT_MODIFY_STATE|SYNCHRONIZE", names[i]);
                check(ok, what);
                if (!ok) Out(L"        %s\n", detail.c_str());
            }
            ev.Close();
        }
        PSECURITY_DESCRIPTOR sd = guard::BuildEventSecurityDescriptor();
        check(sd != nullptr, L"the event security descriptor parses from SDDL");
        if (sd) ::LocalFree(sd);
    }

    // --- argument parsing ----------------------------------------------
    Out(L"\n  -- argument parsing --\n");
    {
        auto parse = [](std::initializer_list<const wchar_t*> args) {
            std::vector<const wchar_t*> v(args);
            return ParseArgs(static_cast<int>(v.size()), v.data());
        };

        ParsedArgs r = parse({ L"edidoverride", L"guarded-apply", L"\\\\.\\DISPLAY1",
                               L"--timeout-seconds", L"25", L"--yes" });
        check(r.ok && r.cmd == L"guarded-apply" && r.target == L"\\\\.\\DISPLAY1" &&
                  r.timeoutSeconds == 25 && r.yes && !r.dryRun,
              L"guarded-apply with a timeout and --yes parses");

        r = parse({ L"edidoverride", L"guarded-apply", L"PHLC310", L"--dry-run" });
        check(r.ok && r.dryRun && !r.yes && r.timeoutSeconds == 10,
              L"--dry-run parses and the timeout defaults to 10");

        r = parse({ L"edidoverride", L"guarded-apply", L"X", L"--timeout-seconds" });
        check(!r.ok, L"--timeout-seconds without a value is rejected");

        r = parse({ L"edidoverride", L"guarded-apply", L"X", L"--timeout-seconds", L"0" });
        check(!r.ok, L"a zero timeout is rejected");

        r = parse({ L"edidoverride", L"guarded-apply", L"X", L"--timeout-seconds", L"9999" });
        check(!r.ok, L"an absurd timeout is rejected");

        r = parse({ L"edidoverride", L"apply", L"X", L"--bogus" });
        check(!r.ok && r.error.find(L"--bogus") != std::wstring::npos,
              L"an unknown flag is rejected and named");

        r = parse({ L"edidoverride", L"--help" });
        check(r.wantsHelp && r.ok, L"--help is recognised");

        r = parse({ L"edidoverride" });
        check(r.wantsHelp && !r.ok, L"no command asks for help and fails");

        r = parse({ L"edidoverride", L"apply", L"X", L"--use-case", L"7", L"--yes" });
        check(r.ok && r.useCase == 7 && r.yes, L"--use-case parses");

        r = parse({ L"edidoverride", L"dump", L"X", L"--hex" });
        check(r.ok && r.hex && r.target == L"X", L"dump --hex parses");
    }

    // --- exit code table ------------------------------------------------
    Out(L"\n  -- exit code contract --\n");
    check(guard::kExitKept == 0 && guard::kExitNotElevated == 2 &&
              guard::kExitApplyFailedReverted == 3 &&
              guard::kExitRestartFailedReverted == 4 && guard::kExitBadArgs == 5 &&
              guard::kExitUserRevert == 10 && guard::kExitTimeoutRevert == 11,
          L"exit codes match the documented contract (0/2/3/4/5/10/11)");

    Out(L"\n  %s (%d failure(s))\n", failures == 0 ? L"ALL CHECKS PASSED" : L"FAILURES",
        failures);
    return failures == 0 ? 0 : 1;
}

bool CopyOverrideKey(const Monitor& m, const wchar_t* from, const wchar_t* to) {
    const std::wstring src = m.regPath + L"\\Device Parameters\\" + from;
    const std::wstring dst = m.regPath + L"\\Device Parameters\\" + to;
    HKEY s = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, src.c_str(), 0, KEY_READ, &s) !=
        ERROR_SUCCESS) {
        return false;
    }
    HKEY d = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, dst.c_str(), 0, nullptr, 0,
                          KEY_WRITE, nullptr, &d, nullptr) != ERROR_SUCCESS) {
        ::RegCloseKey(s);
        return false;
    }
    bool ok = true;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD nameLen = _countof(name);
        DWORD type = 0;
        BYTE  data[1024];
        DWORD cb = sizeof(data);
        const LONG r = ::RegEnumValueW(s, i, name, &nameLen, nullptr, &type, data, &cb);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS) { ok = false; break; }
        if (::RegSetValueExW(d, name, 0, type, data, cb) != ERROR_SUCCESS) {
            ok = false;
            break;
        }
    }
    const DWORD marker = 1;
    ::RegSetValueExW(d, kBackupMarker, 0, REG_DWORD,
                     reinterpret_cast<const BYTE*>(&marker), sizeof(marker));
    ::RegCloseKey(d);
    ::RegCloseKey(s);
    return ok;
}

bool DeleteSubkey(const Monitor& m, const wchar_t* sub) {
    const std::wstring path = m.regPath + L"\\Device Parameters";
    HKEY dp = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_WRITE, &dp) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LONG r = ::RegDeleteTreeW(dp, sub);
    ::RegCloseKey(dp);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

bool DeleteValueIn(const Monitor& m, const wchar_t* sub, const wchar_t* value) {
    const std::wstring path = m.regPath + L"\\Device Parameters\\" + sub;
    HKEY k = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_SET_VALUE, &k) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LONG r = ::RegDeleteValueW(k, value);
    ::RegCloseKey(k);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

// The full device instance id, e.g. "DISPLAY\PHLC310\7&224ecef0&0&UID260".
std::wstring InstanceIdOf(const Monitor& m) {
    return L"DISPLAY\\" + m.hardwareId + L"\\" + m.instanceId;
}

// Backs up any pre-existing override, then writes 'blocks' as EDID_OVERRIDE.
bool WriteOverrideBlocks(const Monitor& m, const Bytes& blocks, bool& backedUp) {
    backedUp = false;
    if (SubkeyExists(m, kOverrideKey) && !SubkeyExists(m, kBackupKey)) {
        if (!CopyOverrideKey(m, kOverrideKey, kBackupKey)) return false;
        backedUp = true;
    }
    const std::wstring path = m.regPath + L"\\Device Parameters\\" + kOverrideKey;
    HKEY k = nullptr;
    if (::RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_WRITE,
                          nullptr, &k, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const size_t count = edid::BlockCount(blocks);
    bool ok = true;
    for (size_t b = 0; b < count; ++b) {
        wchar_t name[16];
        ::_snwprintf_s(name, _countof(name), _TRUNCATE, L"%zu", b);
        if (::RegSetValueExW(k, name, 0, REG_BINARY,
                             blocks.data() + b * edid::kBlockSize,
                             static_cast<DWORD>(edid::kBlockSize)) != ERROR_SUCCESS) {
            ok = false;
            break;
        }
    }
    // A previous override may have had more blocks than ours; leaving stale
    // higher-numbered values behind would corrupt the reassembled EDID.
    for (size_t b = count; b < 32; ++b) {
        wchar_t name[16];
        ::_snwprintf_s(name, _countof(name), _TRUNCATE, L"%zu", b);
        ::RegDeleteValueW(k, name);
    }
    ::RegCloseKey(k);
    return ok;
}

enum class UndoResult { Nothing, RestoredBackup, DeletedOverride, Failed };

const wchar_t* UndoResultName(UndoResult r) {
    switch (r) {
    case UndoResult::RestoredBackup:   return L"restored the previous override";
    case UndoResult::DeletedOverride:  return L"deleted our override";
    case UndoResult::Failed:           return L"FAILED";
    default:                           return L"nothing to undo";
    }
}

// Puts the monitor back the way it was before DesktopSplitter touched it.
// Registry state is re-read live, because a snapshot taken before the write
// would say there is no backup.
UndoResult UndoOverride(const Monitor& m) {
    if (SubkeyExists(m, kBackupKey)) {
        if (!DeleteSubkey(m, kOverrideKey)) return UndoResult::Failed;
        if (!CopyOverrideKey(m, kBackupKey, kOverrideKey)) return UndoResult::Failed;
        DeleteValueIn(m, kOverrideKey, kBackupMarker);   // don't leave our marker
        DeleteSubkey(m, kBackupKey);
        return UndoResult::RestoredBackup;
    }
    if (SubkeyExists(m, kOverrideKey)) {
        if (!DeleteSubkey(m, kOverrideKey)) return UndoResult::Failed;
        return UndoResult::DeletedOverride;
    }
    return UndoResult::Nothing;
}

int CmdApply(const Monitor& m, uint8_t useCase, bool confirmed) {
    if (m.edid.empty()) {
        Out(L"  FAIL: no EDID recorded for this monitor\n");
        return 1;
    }
    edid::VsdbOptions opts;
    opts.primaryUseCase = useCase;
    opts.containerId =
        edid::DeriveContainerId(ContainerSeed(m.hardwareId, m.instanceId));

    const edid::PatchResult pr = edid::AddMicrosoftVsdb(m.edid, opts);
    if (!pr.ok) {
        Out(L"  cannot patch: %S\n", pr.error.c_str());
        return 1;
    }
    const edid::RoundTripReport rep =
        edid::VerifyRoundTrip(m.edid, pr.output, pr.strategy);
    if (!rep.ok) {
        Out(L"  REFUSING to write: round-trip check failed: %S\n", rep.error.c_str());
        return 1;
    }

    Out(L"Planned override for %s\\%s (%s)\n", m.hardwareId.c_str(),
        m.instanceId.c_str(), m.gdiName.empty() ? L"not attached" : m.gdiName.c_str());
    Out(L"  strategy   : %S\n", edid::StrategyName(pr.strategy));
    Out(L"  size       : %zu -> %zu bytes (%zu -> %zu blocks)\n", m.edid.size(),
        pr.output.size(), edid::BlockCount(m.edid), edid::BlockCount(pr.output));
    Out(L"  writes to  : HKLM\\%s\\Device Parameters\\%s\n", m.regPath.c_str(),
        kOverrideKey);
    if (m.hasOverride) {
        Out(L"  NOTE: an EDID override ALREADY EXISTS for this monitor (e.g. from CRU).\n"
            L"        It will be copied to '%s' first and restored by `revert`.\n",
            kBackupKey);
    }

    if (!confirmed) {
        Out(L"\n  Dry run only. Re-run with --yes to write. Read README.md first:\n"
            L"  a bad override can leave this display unusable until the key is\n"
            L"  deleted (possible from Safe Mode).\n");
        return 0;
    }
    if (!IsElevated()) {
        Out(L"\n  FAIL: writing requires an elevated (Administrator) console.\n");
        return 1;
    }

    bool backedUp = false;
    if (!WriteOverrideBlocks(m, pr.output, backedUp)) {
        Out(L"  FAIL: writing the override failed (elevation?).\n");
        return 1;
    }
    if (backedUp) Out(L"  backed up existing override to %s\n", kBackupKey);
    Out(L"\n  Override written (%zu blocks).\n", edid::BlockCount(pr.output));
    Out(L"  Re-enumerate the monitor for it to take effect: unplug/replug it, or\n"
        L"  disable+enable the monitor in Device Manager, or reboot.\n");
    Out(L"  If the display does not come back, see README.md -> RECOVERY.\n");
    return 0;
}

int CmdRevert(const Monitor& m, bool confirmed) {
    Out(L"Revert override for %s\\%s\n", m.hardwareId.c_str(), m.instanceId.c_str());
    if (!m.hasOverride && !m.hasBackup) {
        Out(L"  nothing to do: no override and no backup present.\n");
        return 0;
    }
    if (m.hasBackup) {
        Out(L"  a pre-existing override was backed up; it will be RESTORED.\n");
    } else {
        Out(L"  no backup present; the override key will be DELETED entirely.\n");
    }
    if (!confirmed) {
        Out(L"\n  Dry run only. Re-run with --yes to apply.\n");
        return 0;
    }
    if (!IsElevated()) {
        Out(L"\n  FAIL: this requires an elevated (Administrator) console.\n");
        return 1;
    }
    if (!DeleteSubkey(m, kOverrideKey)) {
        Out(L"  FAIL: could not delete the override key.\n");
        return 1;
    }
    if (m.hasBackup) {
        if (!CopyOverrideKey(m, kBackupKey, kOverrideKey)) {
            Out(L"  FAIL: could not restore the backup. The backup key is still at\n"
                L"        HKLM\\%s\\Device Parameters\\%s\n", m.regPath.c_str(), kBackupKey);
            return 1;
        }
        DeleteSubkey(m, kBackupKey);
        Out(L"  restored the previous override and removed the backup.\n");
    } else {
        Out(L"  override removed.\n");
    }
    Out(L"  Re-enumerate the monitor (replug / disable+enable / reboot).\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Transaction-guarded mode
// ---------------------------------------------------------------------------
//
// This process is the safety authority: it applies the override, restarts the
// monitor, arms a countdown and reverts on its own if nobody confirms - even if
// the GUI that launched it has died. The GUI only ever observes and signals.

int CmdGuardedApply(const Monitor& m, uint8_t useCase, bool confirmed, bool dryRun,
                    int timeoutSeconds) {
    guard::LogOpen(dryRun ? L"guarded-apply --dry-run" : L"guarded-apply");
    auto finish = [](int code) {
        guard::LogClose(code);
        return code;
    };

    if (!dryRun && !confirmed) {
        guard::Log(L"ERROR: guarded-apply needs --yes");
        return finish(guard::kExitBadArgs);
    }
    if (!dryRun && !IsElevated()) {
        guard::Log(L"ERROR: guarded-apply requires an elevated console");
        return finish(guard::kExitNotElevated);
    }

    const std::wstring instanceId = InstanceIdOf(m);
    guard::Log(L"target   : %s", instanceId.c_str());
    guard::Log(L"gdi name : %s",
               m.gdiName.empty() ? L"<not attached>" : m.gdiName.c_str());
    guard::Log(L"timeout  : %d second(s)", timeoutSeconds);
    if (dryRun) {
        guard::Log(L"MODE     : DRY RUN - no registry write, no devnode restart; "
                   L"the events and the countdown are real");
    }

    // --- step a: compute and write the override ---------------------------
    if (m.edid.empty()) {
        guard::Log(L"step a FAILED: no EDID recorded for this monitor");
        return finish(guard::kExitApplyFailedReverted);
    }
    edid::VsdbOptions opts;
    opts.primaryUseCase = useCase;
    opts.containerId =
        edid::DeriveContainerId(ContainerSeed(m.hardwareId, m.instanceId));

    const edid::PatchResult pr = edid::AddMicrosoftVsdb(m.edid, opts);
    if (!pr.ok) {
        guard::Log(L"step a FAILED: %S", pr.error.c_str());
        return finish(guard::kExitApplyFailedReverted);
    }
    const edid::RoundTripReport rep =
        edid::VerifyRoundTrip(m.edid, pr.output, pr.strategy);
    if (!rep.ok) {
        guard::Log(L"step a FAILED: round-trip check rejected the patch: %S",
                   rep.error.c_str());
        return finish(guard::kExitApplyFailedReverted);
    }
    guard::Log(L"step a: patch computed - %S, %zu -> %zu bytes",
               edid::StrategyName(pr.strategy), m.edid.size(), pr.output.size());

    if (dryRun) {
        guard::Log(L"step a: [dry run] would write %zu block(s) to "
                   L"Device Parameters\\%s", edid::BlockCount(pr.output), kOverrideKey);
        if (SubkeyExists(m, kOverrideKey)) {
            guard::Log(L"step a: [dry run] an override already exists and would be "
                       L"backed up to %s first", kBackupKey);
        }
    } else {
        bool backedUp = false;
        if (!WriteOverrideBlocks(m, pr.output, backedUp)) {
            guard::Log(L"step a FAILED: could not write the override; undoing");
            guard::Log(L"  undo: %s", UndoResultName(UndoOverride(m)));
            return finish(guard::kExitApplyFailedReverted);
        }
        guard::Log(L"step a: override written (%zu blocks)%s",
                   edid::BlockCount(pr.output),
                   backedUp ? L", previous override backed up" : L"");
    }

    // Best-effort restoration used by every failure path below.
    auto revertNow = [&](const wchar_t* why) {
        guard::Log(L"REVERTING: %s", why);
        if (dryRun) {
            guard::Log(L"  [dry run] would restore the previous EDID and restart the "
                       L"devnode");
            return;
        }
        guard::Log(L"  undo: %s", UndoResultName(UndoOverride(m)));
        std::wstring err;
        if (!guard::RestartDevnode(instanceId, err)) {
            guard::Log(L"  WARNING: revert restart failed: %s - a reboot will pick up "
                       L"the restored EDID", err.c_str());
        } else if (!guard::WaitDevnodeHealthy(instanceId, 5000, err)) {
            guard::Log(L"  WARNING: monitor unhealthy after the revert restart: %s",
                       err.c_str());
        }
    };

    // --- step b: restart the devnode so the new EDID is read --------------
    if (dryRun) {
        guard::Log(L"step b: [dry run] would disable+enable %s and wait up to 5s for "
                   L"problem code 0", instanceId.c_str());
    } else {
        std::wstring err;
        if (!guard::RestartDevnode(instanceId, err) ||
            !guard::WaitDevnodeHealthy(instanceId, 5000, err)) {
            guard::Log(L"step b FAILED: %s", err.c_str());
            revertNow(L"the monitor did not come back after the restart");
            return finish(guard::kExitRestartFailedReverted);
        }
        guard::Log(L"step b: devnode restarted and healthy");
    }

    // --- step c: arm the countdown ----------------------------------------
    guard::GuardEvents ev;
    if (!ev.Create()) {
        revertNow(L"the guard events could not be created");
        return finish(guard::kExitApplyFailedReverted);
    }
    ::SetEvent(ev.armed);
    guard::Log(L"step c: ARMED - the %d second countdown starts NOW. Waiting for %s "
               L"or %s.", timeoutSeconds, guard::kEventKeep, guard::kEventRevert);

    // --- step d: wait for the verdict -------------------------------------
    HANDLE waits[2] = { ev.keep, ev.revert };
    const DWORD w = ::WaitForMultipleObjects(
        2, waits, FALSE, static_cast<DWORD>(timeoutSeconds) * 1000u);

    int code = guard::kExitTimeoutRevert;
    if (w == WAIT_OBJECT_0) {
        guard::Log(L"step d: KEEP signalled - the override stays");
        if (dryRun) {
            guard::Log(L"  [dry run] would set HidePhysicalDisplay=1");
        } else {
            guard::SetHidePhysicalDisplay(1);
        }
        code = guard::kExitKept;
    } else if (w == WAIT_OBJECT_0 + 1) {
        guard::Log(L"step d: REVERT signalled by the user");
        revertNow(L"the user asked to revert");
        if (dryRun) {
            guard::Log(L"  [dry run] would set HidePhysicalDisplay=0");
        } else {
            guard::SetHidePhysicalDisplay(0);
        }
        code = guard::kExitUserRevert;
    } else {
        guard::Log(L"step d: TIMEOUT after %d second(s) - nobody confirmed",
                   timeoutSeconds);
        revertNow(L"the confirmation window expired");
        if (dryRun) {
            guard::Log(L"  [dry run] would set HidePhysicalDisplay=0");
        } else {
            guard::SetHidePhysicalDisplay(0);
        }
        code = guard::kExitTimeoutRevert;
    }

    ::ResetEvent(ev.armed);
    ev.Close();
    return finish(code);
}

int CmdGuardedRevert(const Monitor& m, bool confirmed, bool dryRun) {
    guard::LogOpen(dryRun ? L"guarded-revert --dry-run" : L"guarded-revert");
    auto finish = [](int code) {
        guard::LogClose(code);
        return code;
    };

    if (!dryRun && !confirmed) {
        guard::Log(L"ERROR: guarded-revert needs --yes");
        return finish(guard::kExitBadArgs);
    }
    if (!dryRun && !IsElevated()) {
        guard::Log(L"ERROR: guarded-revert requires an elevated console");
        return finish(guard::kExitNotElevated);
    }

    const std::wstring instanceId = InstanceIdOf(m);
    guard::Log(L"target: %s", instanceId.c_str());

    Bytes ov;
    const bool haveOverride = ReadOverrideBlocks(m, kOverrideKey, ov);
    const bool haveBackup = SubkeyExists(m, kBackupKey);
    // Only ever delete an override that carries OUR marker (the Microsoft
    // VSDB). An override we did not create - a plain CRU one, say - is the
    // user's and must be left completely alone.
    const bool ourOverride = haveOverride && edid::FindMicrosoftVsdb(ov).found;

    guard::Log(L"state: override=%s backup=%s ours=%s",
               haveOverride ? L"present" : L"none", haveBackup ? L"present" : L"none",
               ourOverride ? L"yes" : L"no");

    if (!haveBackup && !ourOverride) {
        guard::Log(L"nothing of ours to undo - leaving the monitor untouched");
        if (!dryRun) guard::SetHidePhysicalDisplay(0);
        return finish(guard::kExitKept);
    }

    if (dryRun) {
        guard::Log(L"[dry run] would %s, restart %s, and set HidePhysicalDisplay=0",
                   haveBackup ? L"restore the backed-up override"
                              : L"delete our override",
                   instanceId.c_str());
        return finish(guard::kExitKept);
    }

    const UndoResult undo = UndoOverride(m);
    guard::Log(L"undo: %s", UndoResultName(undo));
    if (undo == UndoResult::Failed) {
        guard::Log(L"ERROR: could not undo the override; see README.md RECOVERY");
        return finish(guard::kExitApplyFailedReverted);
    }

    std::wstring err;
    if (!guard::RestartDevnode(instanceId, err)) {
        guard::Log(L"WARNING: devnode restart failed: %s - reboot to pick up the "
                   L"restored EDID", err.c_str());
    } else if (!guard::WaitDevnodeHealthy(instanceId, 5000, err)) {
        guard::Log(L"WARNING: monitor unhealthy after restart: %s", err.c_str());
    }
    guard::SetHidePhysicalDisplay(0);
    return finish(guard::kExitKept);
}

void Usage() {
    Out(L"edidoverride - EDID override helper for DesktopSplitter\n"
        L"\n"
        L"USAGE\n"
        L"  edidoverride list\n"
        L"  edidoverride dump     <target> [--hex]\n"
        L"  edidoverride verify   <target>\n"
        L"  edidoverride selftest <target>\n"
        L"  edidoverride apply    <target> [--use-case N] [--yes]\n"
        L"  edidoverride revert   <target> [--yes]\n"
        L"\n"
        L"GUI-DRIVEN (transaction guarded, one elevation, auto-revert)\n"
        L"  edidoverride guarded-apply  <target> [--timeout-seconds N] [--dry-run] --yes\n"
        L"  edidoverride guarded-revert <target> [--dry-run] --yes\n"
        L"\n"
        L"  guarded-apply writes the override, restarts the monitor devnode, signals\n"
        L"  Global\\DeskSplitEdidArmed and then waits up to N seconds (default 10) for\n"
        L"  Global\\DeskSplitEdidKeep or Global\\DeskSplitEdidRevert. If neither arrives\n"
        L"  it reverts on its own - so a GUI crash, or a user who simply cannot see\n"
        L"  anything, still ends up back where it started.\n"
        L"  --dry-run walks the whole sequence and fires the same events WITHOUT\n"
        L"  writing the registry or restarting anything, and needs no elevation.\n"
        L"\n"
        L"EXIT CODES (guarded-apply / guarded-revert)\n"
        L"   0   kept - the user confirmed, HidePhysicalDisplay=1\n"
        L"   2   not elevated\n"
        L"   3   apply failed; any partial change was reverted\n"
        L"   4   devnode restart failed or the monitor did not return; reverted\n"
        L"   5   bad arguments\n"
        L"  10   the user asked to revert; reverted, HidePhysicalDisplay=0\n"
        L"  11   the countdown expired; auto-reverted, HidePhysicalDisplay=0\n"
        L"\n"
        L"  Every step is logged to stdout AND to\n"
        L"  %ProgramData%\\DesktopSplitter\\edid-guard.log - read that first if the\n"
        L"  display misbehaved and the GUI could not tell you why.\n"
        L"\n"
        L"  <target> is a GDI device name (\\\\.\\DISPLAY1) or any substring of the\n"
        L"  monitor devnode (e.g. PHLC310).\n"
        L"\n"
        L"  apply/revert are DRY RUN unless --yes is given, and need elevation.\n"
        L"  --use-case N sets the VSDB primary use case (default 2 = generic display).\n"
        L"\n"
        L"  selftest reads the real EDID and exercises the patch logic entirely in\n"
        L"  memory. It never writes anything and needs no elevation.\n"
        L"\n"
        L"  !! READ README.md BEFORE USING apply. A bad EDID override can leave a\n"
        L"  !! display unusable. Recovery = delete\n"
        L"  !! HKLM\\SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\<id>\\<inst>\\Device Parameters\\EDID_OVERRIDE\n"
        L"  !! which is possible from Safe Mode.\n");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const ParsedArgs a = ParseArgs(argc, argv);
    if (a.wantsHelp) {
        Usage();
        return a.ok ? 0 : guard::kExitBadArgs;
    }
    if (!a.ok) {
        Out(L"error: %s\n\n", a.error.c_str());
        Usage();
        return guard::kExitBadArgs;
    }

    const bool guarded =
        (a.cmd == L"guarded-apply" || a.cmd == L"guarded-revert");

    const std::vector<Monitor> mons = EnumerateMonitors();
    if (a.cmd == L"list") return CmdList(mons);

    if (a.target.empty()) {
        Out(L"error: this command needs a <target>. Run `edidoverride list`.\n");
        return guard::kExitBadArgs;
    }
    const Monitor* m = ResolveTarget(mons, a.target);
    if (!m) {
        Out(L"error: no monitor matched '%s'. Run `edidoverride list`.\n",
            a.target.c_str());
        return guard::kExitBadArgs;
    }

    if (a.cmd == L"dump")     return CmdDump(*m, a.hex);
    if (a.cmd == L"verify")   return CmdVerify(*m);
    if (a.cmd == L"selftest") return CmdSelfTest(*m, a.useCase);
    if (a.cmd == L"apply")    return CmdApply(*m, a.useCase, a.yes);
    if (a.cmd == L"revert")   return CmdRevert(*m, a.yes);
    if (a.cmd == L"guarded-apply") {
        return CmdGuardedApply(*m, a.useCase, a.yes, a.dryRun, a.timeoutSeconds);
    }
    if (a.cmd == L"guarded-revert") return CmdGuardedRevert(*m, a.yes, a.dryRun);

    (void)guarded;
    Out(L"unknown command '%s'\n\n", a.cmd.c_str());
    Usage();
    return guard::kExitBadArgs;
}
