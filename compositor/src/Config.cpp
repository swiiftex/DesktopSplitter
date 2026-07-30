#include "Config.h"
#include "Json.h"
#include "Log.h"

#include <cstdio>

namespace ds {

std::wstring DefaultConfigPath() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::ExpandEnvironmentStringsW(
        L"%ProgramData%\\DesktopSplitter\\config.json", buf, _countof(buf));
    if (n == 0 || n > _countof(buf)) {
        return L"C:\\ProgramData\\DesktopSplitter\\config.json";
    }
    return std::wstring(buf);
}

static bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DS_ERR(L"cannot open config '%s': %s", path.c_str(),
               HrString(HRESULT_FROM_WIN32(::GetLastError())));
        return false;
    }
    LARGE_INTEGER size = {};
    if (!::GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (16 * 1024 * 1024)) {
        DS_ERR(L"config file has an implausible size");
        ::CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ::ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &read, nullptr);
    ::CloseHandle(h);
    if (!ok) {
        DS_ERR(L"failed reading config file");
        return false;
    }
    out.resize(read);
    return true;
}

bool LoadConfig(const std::wstring& path, AppConfig& out) {
    std::string text;
    if (!ReadWholeFile(path, text)) return false;

    json::Value root;
    std::string err;
    if (!json::Parse(text, root, err)) {
        DS_ERR(L"config JSON parse error: %s", Utf8ToWide(err).c_str());
        return false;
    }
    if (!root.isObject()) {
        DS_ERR(L"config root is not a JSON object");
        return false;
    }

    out = AppConfig();
    out.physicalDevice = Utf8ToWide(root.stringMember("physicalDevice"));
    out.refreshMillihertz = root.uintMember("refreshMillihertz", 60000);
    out.primarySegment = root.uintMember("primarySegment", 0);
    out.launchSegment = root.intMember("launchSegment", -1);
    if (out.refreshMillihertz < 1000) out.refreshMillihertz = 60000;

    if (out.physicalDevice.empty()) {
        DS_ERR(L"config is missing 'physicalDevice'");
        return false;
    }

    const json::Value* segs = root.find("segments");
    if (segs == nullptr || !segs->isArray() || segs->size() == 0) {
        DS_ERR(L"config is missing a non-empty 'segments' array");
        return false;
    }

    for (size_t i = 0; i < segs->size(); ++i) {
        const json::Value& s = segs->at(i);
        if (!s.isObject()) {
            DS_ERR(L"segments[%zu] is not an object", i);
            return false;
        }
        SegmentConfig sc;
        sc.virtualDevice = Utf8ToWide(s.stringMember("virtualDevice"));
        // Absent means "show" - never hide a taskbar because a key was missing.
        const json::Value* st = s.find("showTaskbar");
        sc.showTaskbar = (st && st->isBool()) ? st->asBool(true) : true;
        sc.width  = s.uintMember("width", 0);
        sc.height = s.uintMember("height", 0);

        const json::Value* pr = s.find("physRect");
        if (pr == nullptr || !pr->isObject()) {
            DS_ERR(L"segments[%zu] is missing 'physRect'", i);
            return false;
        }
        sc.physRect.x = pr->intMember("x", 0);
        sc.physRect.y = pr->intMember("y", 0);
        sc.physRect.w = pr->intMember("w", 0);
        sc.physRect.h = pr->intMember("h", 0);

        if (sc.virtualDevice.empty()) {
            DS_ERR(L"segments[%zu] is missing 'virtualDevice'", i);
            return false;
        }
        if (sc.physRect.w <= 0 || sc.physRect.h <= 0) {
            DS_ERR(L"segments[%zu] has an empty physRect", i);
            return false;
        }
        out.segments.push_back(std::move(sc));
    }

    DS_LOG(L"config: physical=%s refresh=%u mHz segments=%zu",
           out.physicalDevice.c_str(), out.refreshMillihertz, out.segments.size());
    for (size_t i = 0; i < out.segments.size(); ++i) {
        const SegmentConfig& s = out.segments[i];
        DS_VERB(L"  segment %zu: %s %ux%u -> physRect(%d,%d,%d,%d) taskbar=%s%s",
                i, s.virtualDevice.c_str(), s.width, s.height, s.physRect.x,
                s.physRect.y, s.physRect.w, s.physRect.h,
                s.showTaskbar ? L"show" : L"HIDE",
                (i == out.primarySegment) ? L" (primary)" : L"");
    }
    return true;
}

} // namespace ds
