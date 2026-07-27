// Trace.h - minimal logging for DesktopSplitterVdd.
//
// The Microsoft IddSampleDriver uses WPP software tracing. WPP is deliberately
// NOT used here: it requires a WPP configuration block plus the tracewpp
// pre-processing step, which makes the project harder to build and adds no
// value for this driver. Messages go to the kernel/user debugger via
// OutputDebugString instead (visible in DebugView with "Capture Global Win32").
#pragma once

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#ifndef DESKSPLIT_TRACE_ENABLED
#define DESKSPLIT_TRACE_ENABLED 1
#endif

inline void DeskSplitTraceImpl(_In_z_ const char* Level, _In_z_ _Printf_format_string_ const char* Format, ...)
{
#if DESKSPLIT_TRACE_ENABLED
    char Body[512];
    char Line[600];

    va_list Args;
    va_start(Args, Format);
    _vsnprintf_s(Body, sizeof(Body), _TRUNCATE, Format, Args);
    va_end(Args);

    _snprintf_s(Line, sizeof(Line), _TRUNCATE, "[DesktopSplitterVdd] %s: %s\n", Level, Body);
    OutputDebugStringA(Line);
#else
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Format);
#endif
}

#define DS_LOG(level, ...)  DeskSplitTraceImpl(level, __VA_ARGS__)
#define DS_ERROR(...)       DS_LOG("ERROR", __VA_ARGS__)
#define DS_WARN(...)        DS_LOG("WARN ", __VA_ARGS__)
#define DS_INFO(...)        DS_LOG("INFO ", __VA_ARGS__)
