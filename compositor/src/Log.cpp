#include "Log.h"

#include <cstdio>
#include <cstdarg>
#include <comdef.h>

namespace ds {

namespace {
bool g_verbose = false;
}

void LogInit(bool verbose) {
    g_verbose = verbose;
}

bool LogIsVerbose() {
    return g_verbose;
}

void LogWrite(const wchar_t* level, const wchar_t* fmt, ...) {
    wchar_t body[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME st;
    ::GetLocalTime(&st);

    wchar_t line[1200];
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"[%02u:%02u:%02u.%03u] [%s] [t%lu] %s\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 level, ::GetCurrentThreadId(), body);

    ::OutputDebugStringW(line);
    ::fputws(line, stderr);
    ::fflush(stderr);
}

const wchar_t* HrString(HRESULT hr) {
    static thread_local wchar_t buf[512];
    wchar_t* msg = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    if (n && msg) {
        // Strip trailing CR/LF.
        for (DWORD i = n; i > 0; --i) {
            if (msg[i - 1] == L'\r' || msg[i - 1] == L'\n' || msg[i - 1] == L' ')
                msg[i - 1] = L'\0';
            else
                break;
        }
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"0x%08X (%s)",
                     static_cast<unsigned>(hr), msg);
        ::LocalFree(msg);
    } else {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"0x%08X",
                     static_cast<unsigned>(hr));
    }
    return buf;
}

} // namespace ds
