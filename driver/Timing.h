// Timing.h - CVT reduced-blanking (CVT-RB v1) style timing generation and
// mode validation for DesktopSplitterVdd.
//
// A single helper produces the horizontal/vertical totals and the pixel clock
// that are used consistently by BOTH:
//   * DISPLAYCONFIG_VIDEO_SIGNAL_INFO (IDDCX_MONITOR_MODE / IDDCX_TARGET_MODE)
//   * the EDID detailed timing descriptor
// so the two never disagree.
#pragma once

#include <windows.h>
#include <winioctl.h>

#include "DeskSplitProtocol.h"

// Mode sanity limits enforced on IOCTL_DESKSPLIT_SET_CONFIG.
#define DESKSPLIT_MIN_DIMENSION     640u
#define DESKSPLIT_MAX_DIMENSION     7680u
#define DESKSPLIT_MIN_REFRESH_MHZ   23000u        // 23.000 Hz
#define DESKSPLIT_MAX_REFRESH_MHZ   1000000u      // 1000.000 Hz

typedef struct DESKSPLIT_TIMING
{
    UINT32 HActive;
    UINT32 HFrontPorch;
    UINT32 HSyncWidth;
    UINT32 HBackPorch;
    UINT32 HBlank;
    UINT32 HTotal;

    UINT32 VActive;
    UINT32 VFrontPorch;
    UINT32 VSyncWidth;
    UINT32 VBackPorch;
    UINT32 VBlank;
    UINT32 VTotal;

    UINT64 PixelClockHz;        // hTotal * vTotal * vRefresh

    UINT32 VSyncNumerator;      // vertical refresh as a reduced rational, in Hz
    UINT32 VSyncDenominator;

    UINT32 HSyncNumerator;      // horizontal rate as a reduced rational, in Hz
    UINT32 HSyncDenominator;
} DESKSPLIT_TIMING;

// Returns FALSE if the mode is outside the supported limits (in which case
// *pTiming is not written).
_Success_(return != false)
bool DeskSplitComputeTiming(
    _In_ UINT32 Width,
    _In_ UINT32 Height,
    _In_ UINT32 RefreshMillihertz,
    _Out_ DESKSPLIT_TIMING* pTiming);

// Range/sanity check only (no timing generation).
bool DeskSplitIsModeSane(_In_ const DESKSPLIT_MODE& Mode);

// Validates a whole DESKSPLIT_CONFIG blob received over the IOCTL interface or
// read back from the registry.
bool DeskSplitIsConfigValid(_In_ const DESKSPLIT_CONFIG& Config);
