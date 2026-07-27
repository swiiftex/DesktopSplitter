// Common.h - shared includes and small helper types for dscomp.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// Rectangle in physical-monitor-local pixels (from config.json "physRect").
struct PhysRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct SegmentConfig {
    std::wstring virtualDevice;   // e.g. L"\\\\.\\DISPLAY3"
    uint32_t     width = 0;       // expected capture width  (informational)
    uint32_t     height = 0;      // expected capture height (informational)
    PhysRect     physRect;
};

struct AppConfig {
    std::wstring               physicalDevice;          // e.g. L"\\\\.\\DISPLAY1"
    uint32_t                   refreshMillihertz = 60000;
    std::vector<SegmentConfig> segments;
};

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                           static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                          &out[0], need);
    return out;
}

} // namespace ds
