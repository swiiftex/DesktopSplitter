#include "Timing.h"

#include <math.h>

namespace
{
    // CVT reduced blanking v1 constants (VESA CVT 1.2, reduced blanking timing).
    constexpr UINT32 kRbHFrontPorch = 48;
    constexpr UINT32 kRbHSyncWidth = 32;
    constexpr UINT32 kRbHBackPorch = 80;
    constexpr UINT32 kRbHBlank = kRbHFrontPorch + kRbHSyncWidth + kRbHBackPorch;   // 160
    constexpr UINT32 kRbVFrontPorch = 3;
    constexpr UINT32 kRbVBackPorch = 6;
    constexpr double kRbMinVBlankMicroseconds = 460.0;

    // EDID detailed timing descriptor field widths (12 bits each).
    constexpr UINT32 kEdidMaxDtdField = 4095;

    UINT64 Gcd64(UINT64 A, UINT64 B)
    {
        while (B != 0)
        {
            UINT64 T = A % B;
            A = B;
            B = T;
        }
        return (A != 0) ? A : 1;
    }

    // CVT vertical sync width, selected from the aspect ratio.
    UINT32 VSyncWidthForAspect(UINT32 Width, UINT32 Height)
    {
        if (Height == 0)
        {
            return 10;
        }

        const double Aspect = static_cast<double>(Width) / static_cast<double>(Height);
        const double Tolerance = 0.02;

        if (fabs(Aspect - (4.0 / 3.0)) < Tolerance)   return 4;
        if (fabs(Aspect - (16.0 / 9.0)) < Tolerance)  return 5;
        if (fabs(Aspect - (16.0 / 10.0)) < Tolerance) return 6;
        if (fabs(Aspect - (5.0 / 4.0)) < Tolerance)   return 7;
        if (fabs(Aspect - (15.0 / 9.0)) < Tolerance)  return 7;

        // Not one of the CVT standard aspect ratios; CVT leaves this undefined,
        // 10 lines is a safe generic value.
        return 10;
    }

    void ReduceRational(UINT64 Numerator, UINT64 Denominator, UINT32* pNumOut, UINT32* pDenOut)
    {
        if (Denominator == 0)
        {
            Denominator = 1;
        }

        const UINT64 Divisor = Gcd64(Numerator, Denominator);
        Numerator /= Divisor;
        Denominator /= Divisor;

        // DISPLAYCONFIG_RATIONAL is a pair of UINT32. Scale down if the reduced
        // fraction still does not fit (only reachable at absurd refresh rates).
        constexpr UINT64 kMaxUint32 = 0xFFFFFFFFull;

        while ((Numerator > kMaxUint32 || Denominator > kMaxUint32) && Denominator > 1)
        {
            Numerator >>= 1;
            Denominator >>= 1;
        }

        if (Numerator > kMaxUint32)
        {
            Numerator = kMaxUint32;
        }
        if (Denominator == 0)
        {
            Denominator = 1;
        }

        *pNumOut = static_cast<UINT32>(Numerator);
        *pDenOut = static_cast<UINT32>(Denominator);
    }
}

_Use_decl_annotations_
bool DeskSplitIsModeSane(const DESKSPLIT_MODE& Mode)
{
    if (Mode.Width < DESKSPLIT_MIN_DIMENSION || Mode.Width > DESKSPLIT_MAX_DIMENSION)
    {
        return false;
    }
    if (Mode.Height < DESKSPLIT_MIN_DIMENSION || Mode.Height > DESKSPLIT_MAX_DIMENSION)
    {
        return false;
    }
    if (Mode.RefreshMillihertz < DESKSPLIT_MIN_REFRESH_MHZ || Mode.RefreshMillihertz > DESKSPLIT_MAX_REFRESH_MHZ)
    {
        return false;
    }
    return true;
}

_Use_decl_annotations_
bool DeskSplitIsConfigValid(const DESKSPLIT_CONFIG& Config)
{
    if (Config.Version != DESKSPLIT_PROTOCOL_VERSION)
    {
        return false;
    }
    if (Config.MonitorCount > DESKSPLIT_MAX_MONITORS)
    {
        return false;
    }

    for (UINT32 MonitorIndex = 0; MonitorIndex < Config.MonitorCount; ++MonitorIndex)
    {
        const DESKSPLIT_MONITOR_CONFIG& Monitor = Config.Monitors[MonitorIndex];

        if (Monitor.ModeCount == 0 || Monitor.ModeCount > DESKSPLIT_MAX_MODES)
        {
            return false;
        }
        if (Monitor.PreferredModeIndex >= Monitor.ModeCount)
        {
            return false;
        }

        for (UINT32 ModeIndex = 0; ModeIndex < Monitor.ModeCount; ++ModeIndex)
        {
            if (!DeskSplitIsModeSane(Monitor.Modes[ModeIndex]))
            {
                return false;
            }
        }
    }

    return true;
}

_Use_decl_annotations_
bool DeskSplitComputeTiming(UINT32 Width, UINT32 Height, UINT32 RefreshMillihertz, DESKSPLIT_TIMING* pTiming)
{
    if (pTiming == nullptr)
    {
        return false;
    }

    DESKSPLIT_MODE Mode = {};
    Mode.Width = Width;
    Mode.Height = Height;
    Mode.RefreshMillihertz = RefreshMillihertz;
    if (!DeskSplitIsModeSane(Mode))
    {
        return false;
    }

    ZeroMemory(pTiming, sizeof(*pTiming));

    // ---- Horizontal ------------------------------------------------------
    // CVT rounds the active width down to an 8 pixel cell boundary; we do NOT,
    // because the active size has to be exactly the resolution we advertise.
    // Only the blanking is taken from CVT-RB.
    pTiming->HActive = Width;
    pTiming->HFrontPorch = kRbHFrontPorch;
    pTiming->HSyncWidth = kRbHSyncWidth;
    pTiming->HBackPorch = kRbHBackPorch;
    pTiming->HBlank = kRbHBlank;
    pTiming->HTotal = Width + kRbHBlank;

    // ---- Vertical --------------------------------------------------------
    const UINT32 VSyncWidth = VSyncWidthForAspect(Width, Height);
    const UINT32 MinVBlank = kRbVFrontPorch + VSyncWidth + kRbVBackPorch;

    const double RefreshHz = static_cast<double>(RefreshMillihertz) / 1000.0;
    const double FramePeriodMicroseconds = 1000000.0 / RefreshHz;

    UINT32 VBlank = MinVBlank;

    // Estimated line period with the minimum vertical blanking interval.
    const double LinePeriodMicroseconds =
        (FramePeriodMicroseconds - kRbMinVBlankMicroseconds) / static_cast<double>(Height);

    if (LinePeriodMicroseconds > 0.0)
    {
        const double BlankLines = ceil(kRbMinVBlankMicroseconds / LinePeriodMicroseconds);
        if (BlankLines > static_cast<double>(MinVBlank))
        {
            VBlank = (BlankLines > static_cast<double>(kEdidMaxDtdField))
                ? kEdidMaxDtdField
                : static_cast<UINT32>(BlankLines);
        }
    }

    pTiming->VActive = Height;
    pTiming->VFrontPorch = kRbVFrontPorch;
    pTiming->VSyncWidth = VSyncWidth;
    pTiming->VBlank = VBlank;
    pTiming->VBackPorch = VBlank - kRbVFrontPorch - VSyncWidth;
    pTiming->VTotal = Height + VBlank;

    // ---- Pixel clock -----------------------------------------------------
    // pixelClock[Hz] = hTotal * vTotal * refresh[Hz]
    //               = hTotal * vTotal * refreshMillihertz / 1000
    const UINT64 ClockNumerator =
        static_cast<UINT64>(RefreshMillihertz) *
        static_cast<UINT64>(pTiming->HTotal) *
        static_cast<UINT64>(pTiming->VTotal);
    pTiming->PixelClockHz = (ClockNumerator + 500) / 1000;

    // ---- Refresh rationals ----------------------------------------------
    // e.g. 164999 mHz -> 164999 / 1000 (gcd 1); 60000 mHz -> 60 / 1.
    ReduceRational(RefreshMillihertz, 1000, &pTiming->VSyncNumerator, &pTiming->VSyncDenominator);

    // hSyncFreq = vSyncFreq * vTotal
    ReduceRational(
        static_cast<UINT64>(RefreshMillihertz) * static_cast<UINT64>(pTiming->VTotal),
        1000,
        &pTiming->HSyncNumerator,
        &pTiming->HSyncDenominator);

    return true;
}
