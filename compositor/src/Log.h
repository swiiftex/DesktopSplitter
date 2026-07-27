// Log.h - minimal logging to stderr + OutputDebugString.
#pragma once

#include "Common.h"

namespace ds {

void LogInit(bool verbose);
bool LogIsVerbose();
void LogWrite(const wchar_t* level, const wchar_t* fmt, ...);

// Formats an HRESULT as "0x........ (message)". Result points at a
// thread-local buffer that is valid until the next call on this thread.
const wchar_t* HrString(HRESULT hr);

} // namespace ds

#define DS_LOG(fmt, ...)  ::ds::LogWrite(L"info ", fmt, ##__VA_ARGS__)
#define DS_WARN(fmt, ...) ::ds::LogWrite(L"warn ", fmt, ##__VA_ARGS__)
#define DS_ERR(fmt, ...)  ::ds::LogWrite(L"error", fmt, ##__VA_ARGS__)
#define DS_VERB(fmt, ...)                                                     \
    do {                                                                      \
        if (::ds::LogIsVerbose()) ::ds::LogWrite(L"debug", fmt, ##__VA_ARGS__); \
    } while (0)
