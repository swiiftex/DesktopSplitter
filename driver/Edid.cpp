#include "Edid.h"

#include <stdio.h>
#include <string.h>

namespace
{
    // EDID detailed timing descriptor fields are 12 bits wide; anything wider
    // simply cannot be expressed in a DTD.
    constexpr UINT32 kEdidMaxDtdField = 4095;

    // Pixel clock in a DTD is stored in 10 kHz units in a 16 bit field.
    constexpr UINT32 kEdidMaxDtdClock10kHz = 0xFFFF;

    bool TimingFitsInDtd(const DESKSPLIT_TIMING& Timing)
    {
        return Timing.HActive <= kEdidMaxDtdField &&
               Timing.HBlank <= kEdidMaxDtdField &&
               Timing.VActive <= kEdidMaxDtdField &&
               Timing.VBlank <= kEdidMaxDtdField;
    }

    // Approximate physical size at a nominal 96 DPI, clamped to the EDID byte
    // range (1..255 cm).
    void PhysicalSizeCm(UINT32 Width, UINT32 Height, BYTE* pWidthCm, BYTE* pHeightCm)
    {
        UINT32 WidthCm = (Width * 254u) / (96u * 100u);
        UINT32 HeightCm = (Height * 254u) / (96u * 100u);

        if (WidthCm < 1) WidthCm = 1;
        if (HeightCm < 1) HeightCm = 1;
        if (WidthCm > 255) WidthCm = 255;
        if (HeightCm > 255) HeightCm = 255;

        *pWidthCm = static_cast<BYTE>(WidthCm);
        *pHeightCm = static_cast<BYTE>(HeightCm);
    }

    // 18 byte detailed timing descriptor.
    void WriteDetailedTimingDescriptor(BYTE* p, const DESKSPLIT_TIMING& T, UINT32 WidthMm, UINT32 HeightMm)
    {
        UINT32 Clock10kHz = static_cast<UINT32>((T.PixelClockHz + 5000) / 10000);
        if (Clock10kHz == 0)
        {
            Clock10kHz = 1;                        // 0 would mark this as a display descriptor
        }
        if (Clock10kHz > kEdidMaxDtdClock10kHz)
        {
            Clock10kHz = kEdidMaxDtdClock10kHz;    // EDID 1.4 cannot express > 655.35 MHz
        }

        const UINT32 HActive = T.HActive & 0xFFF;
        const UINT32 HBlank = T.HBlank & 0xFFF;
        const UINT32 VActive = T.VActive & 0xFFF;
        const UINT32 VBlank = T.VBlank & 0xFFF;
        const UINT32 HFront = T.HFrontPorch & 0x3FF;
        const UINT32 HSync = T.HSyncWidth & 0x3FF;
        const UINT32 VFront = T.VFrontPorch & 0x3F;
        const UINT32 VSync = T.VSyncWidth & 0x3F;

        p[0] = static_cast<BYTE>(Clock10kHz & 0xFF);
        p[1] = static_cast<BYTE>((Clock10kHz >> 8) & 0xFF);
        p[2] = static_cast<BYTE>(HActive & 0xFF);
        p[3] = static_cast<BYTE>(HBlank & 0xFF);
        p[4] = static_cast<BYTE>(((HActive >> 8) << 4) | (HBlank >> 8));
        p[5] = static_cast<BYTE>(VActive & 0xFF);
        p[6] = static_cast<BYTE>(VBlank & 0xFF);
        p[7] = static_cast<BYTE>(((VActive >> 8) << 4) | (VBlank >> 8));
        p[8] = static_cast<BYTE>(HFront & 0xFF);
        p[9] = static_cast<BYTE>(HSync & 0xFF);
        p[10] = static_cast<BYTE>(((VFront & 0x0F) << 4) | (VSync & 0x0F));
        p[11] = static_cast<BYTE>(((HFront >> 8) << 6) |
                                  ((HSync >> 8) << 4) |
                                  ((VFront >> 4) << 2) |
                                  (VSync >> 4));
        p[12] = static_cast<BYTE>(WidthMm & 0xFF);
        p[13] = static_cast<BYTE>(HeightMm & 0xFF);
        p[14] = static_cast<BYTE>((((WidthMm >> 8) & 0x0F) << 4) | ((HeightMm >> 8) & 0x0F));
        p[15] = 0;      // horizontal border
        p[16] = 0;      // vertical border
        // Digital separate sync, vertical sync negative, horizontal sync positive
        // (the CVT reduced-blanking polarity convention).
        p[17] = 0x1A;
    }

    // 18 byte display descriptor carrying ASCII text (monitor name / serial).
    void WriteTextDescriptor(BYTE* p, BYTE Tag, const char* Text)
    {
        p[0] = 0x00;
        p[1] = 0x00;
        p[2] = 0x00;
        p[3] = Tag;
        p[4] = 0x00;

        int Index = 0;
        for (; Index < 13 && Text[Index] != '\0'; ++Index)
        {
            p[5 + Index] = static_cast<BYTE>(Text[Index]);
        }
        if (Index < 13)
        {
            p[5 + Index] = 0x0A;        // terminator
            ++Index;
        }
        for (; Index < 13; ++Index)
        {
            p[5 + Index] = 0x20;        // pad with spaces
        }
    }

    // 18 byte display range limits descriptor (tag 0xFD).
    void WriteRangeLimitsDescriptor(
        BYTE* p,
        UINT32 MinVerticalHz,
        UINT32 MaxVerticalHz,
        UINT32 MinHorizontalKHz,
        UINT32 MaxHorizontalKHz,
        UINT32 MaxPixelClockMHz)
    {
        auto ClampByte = [](UINT32 Value) -> BYTE
        {
            if (Value < 1)   Value = 1;
            if (Value > 255) Value = 255;
            return static_cast<BYTE>(Value);
        };

        p[0] = 0x00;
        p[1] = 0x00;
        p[2] = 0x00;
        p[3] = 0xFD;
        p[4] = 0x00;                                        // no rate offsets
        p[5] = ClampByte(MinVerticalHz);
        p[6] = ClampByte(MaxVerticalHz);
        p[7] = ClampByte(MinHorizontalKHz);
        p[8] = ClampByte(MaxHorizontalKHz);
        p[9] = ClampByte((MaxPixelClockMHz + 9) / 10);      // in 10 MHz units
        p[10] = 0x01;                                       // range limits only, no timing formula
        p[11] = 0x0A;
        p[12] = 0x20;
        p[13] = 0x20;
        p[14] = 0x20;
        p[15] = 0x20;
        p[16] = 0x20;
        p[17] = 0x20;
    }
}

_Use_decl_annotations_
bool DeskSplitBuildEdid(UINT32 MonitorIndex, const DESKSPLIT_MONITOR_CONFIG& Config, BYTE* pEdid)
{
    if (pEdid == nullptr || MonitorIndex >= DESKSPLIT_MAX_MONITORS)
    {
        return false;
    }
    if (Config.ModeCount == 0 || Config.ModeCount > DESKSPLIT_MAX_MODES)
    {
        return false;
    }
    if (Config.PreferredModeIndex >= Config.ModeCount)
    {
        return false;
    }

    // ---- Survey the mode list -------------------------------------------
    DESKSPLIT_TIMING PreferredTiming = {};
    if (!DeskSplitComputeTiming(
            Config.Modes[Config.PreferredModeIndex].Width,
            Config.Modes[Config.PreferredModeIndex].Height,
            Config.Modes[Config.PreferredModeIndex].RefreshMillihertz,
            &PreferredTiming))
    {
        return false;
    }

    UINT32 MinVerticalHz = 0xFFFFFFFF;
    UINT32 MaxVerticalHz = 0;
    UINT32 MinHorizontalKHz = 0xFFFFFFFF;
    UINT32 MaxHorizontalKHz = 0;
    UINT32 MaxPixelClockMHz = 0;

    // Detailed timing to publish: the preferred mode if it can be expressed in
    // a DTD, otherwise the first mode that can. (Windows takes the real mode
    // list from EvtIddCxParseMonitorDescription / EvtIddCxMonitorQueryTargetModes,
    // so the DTD only has to be structurally valid and plausible.)
    DESKSPLIT_TIMING DtdTiming = PreferredTiming;
    bool HaveDtd = TimingFitsInDtd(PreferredTiming);

    for (UINT32 ModeIndex = 0; ModeIndex < Config.ModeCount; ++ModeIndex)
    {
        DESKSPLIT_TIMING Timing = {};
        if (!DeskSplitComputeTiming(
                Config.Modes[ModeIndex].Width,
                Config.Modes[ModeIndex].Height,
                Config.Modes[ModeIndex].RefreshMillihertz,
                &Timing))
        {
            return false;
        }

        const UINT32 VerticalHz = (Config.Modes[ModeIndex].RefreshMillihertz + 999) / 1000;
        const UINT32 HorizontalKHz =
            static_cast<UINT32>(((static_cast<UINT64>(Config.Modes[ModeIndex].RefreshMillihertz) *
                                  Timing.VTotal) + 999999) / 1000000);
        const UINT32 PixelClockMHz = static_cast<UINT32>((Timing.PixelClockHz + 999999) / 1000000);

        if (VerticalHz < MinVerticalHz)      MinVerticalHz = VerticalHz;
        if (VerticalHz > MaxVerticalHz)      MaxVerticalHz = VerticalHz;
        if (HorizontalKHz < MinHorizontalKHz) MinHorizontalKHz = HorizontalKHz;
        if (HorizontalKHz > MaxHorizontalKHz) MaxHorizontalKHz = HorizontalKHz;
        if (PixelClockMHz > MaxPixelClockMHz) MaxPixelClockMHz = PixelClockMHz;

        if (!HaveDtd && TimingFitsInDtd(Timing))
        {
            DtdTiming = Timing;
            HaveDtd = true;
        }
    }

    if (!HaveDtd)
    {
        // No configured mode fits a DTD (e.g. every mode is wider than 4095
        // pixels). Publish a generic 1920x1080@60 DTD so the block stays valid.
        if (!DeskSplitComputeTiming(1920, 1080, 60000, &DtdTiming))
        {
            return false;
        }
    }

    // ---- Header ----------------------------------------------------------
    ZeroMemory(pEdid, DESKSPLIT_EDID_SIZE);

    pEdid[0] = 0x00;
    pEdid[1] = 0xFF;
    pEdid[2] = 0xFF;
    pEdid[3] = 0xFF;
    pEdid[4] = 0xFF;
    pEdid[5] = 0xFF;
    pEdid[6] = 0xFF;
    pEdid[7] = 0x00;

    // Manufacturer ID "DSP".
    pEdid[8] = DESKSPLIT_EDID_MFG_BYTE0;
    pEdid[9] = DESKSPLIT_EDID_MFG_BYTE1;

    // Product code = monitor index (little endian).
    const UINT16 ProductCode = static_cast<UINT16>(MonitorIndex);
    pEdid[10] = static_cast<BYTE>(ProductCode & 0xFF);
    pEdid[11] = static_cast<BYTE>((ProductCode >> 8) & 0xFF);

    // Serial number, unique per monitor index (little endian).
    const UINT32 SerialNumber = 0xD5000001u + MonitorIndex;
    pEdid[12] = static_cast<BYTE>(SerialNumber & 0xFF);
    pEdid[13] = static_cast<BYTE>((SerialNumber >> 8) & 0xFF);
    pEdid[14] = static_cast<BYTE>((SerialNumber >> 16) & 0xFF);
    pEdid[15] = static_cast<BYTE>((SerialNumber >> 24) & 0xFF);

    pEdid[16] = 1;          // week of manufacture
    pEdid[17] = 34;         // year of manufacture: 1990 + 34 = 2024

    pEdid[18] = 1;          // EDID version 1
    pEdid[19] = 4;          // EDID revision 4

    // ---- Basic display parameters ---------------------------------------
    // Digital input, 8 bits per colour, DisplayPort interface.
    pEdid[20] = 0xA5;

    BYTE WidthCm = 0;
    BYTE HeightCm = 0;
    PhysicalSizeCm(PreferredTiming.HActive, PreferredTiming.VActive, &WidthCm, &HeightCm);
    pEdid[21] = WidthCm;
    pEdid[22] = HeightCm;

    pEdid[23] = 120;        // display gamma: (2.20 * 100) - 100

    // Feature support: sRGB colour space, preferred timing mode includes the
    // native pixel format and refresh rate (required by EDID 1.4).
    pEdid[24] = 0x06;

    // ---- Chromaticity (sRGB primaries) ----------------------------------
    pEdid[25] = 0xEE;
    pEdid[26] = 0x91;
    pEdid[27] = 0xA3;
    pEdid[28] = 0x54;
    pEdid[29] = 0x4C;
    pEdid[30] = 0x99;
    pEdid[31] = 0x26;
    pEdid[32] = 0x0F;
    pEdid[33] = 0x50;
    pEdid[34] = 0x54;

    // ---- Established / standard timings ---------------------------------
    // None advertised here: the OS gets the real mode list from the IddCx
    // callbacks, and advertising bogus legacy timings would only add modes we
    // do not want.
    pEdid[35] = 0x00;
    pEdid[36] = 0x00;
    pEdid[37] = 0x00;

    for (int Index = 0; Index < 8; ++Index)
    {
        pEdid[38 + (Index * 2)] = 0x01;      // unused standard timing
        pEdid[39 + (Index * 2)] = 0x01;
    }

    // ---- Descriptors -----------------------------------------------------
    const UINT32 WidthMm = static_cast<UINT32>(WidthCm) * 10u;
    const UINT32 HeightMm = static_cast<UINT32>(HeightCm) * 10u;

    WriteDetailedTimingDescriptor(&pEdid[54], DtdTiming, WidthMm, HeightMm);

    WriteRangeLimitsDescriptor(
        &pEdid[72],
        MinVerticalHz,
        MaxVerticalHz,
        MinHorizontalKHz,
        MaxHorizontalKHz,
        MaxPixelClockMHz);

    char MonitorName[16] = {};
    _snprintf_s(MonitorName, sizeof(MonitorName), _TRUNCATE, "DeskSplit %u", MonitorIndex + 1);
    WriteTextDescriptor(&pEdid[90], 0xFC, MonitorName);

    char SerialText[16] = {};
    _snprintf_s(SerialText, sizeof(SerialText), _TRUNCATE, "DSP%08X", SerialNumber);
    WriteTextDescriptor(&pEdid[108], 0xFF, SerialText);

    // ---- Extension count + checksum -------------------------------------
    pEdid[126] = 0x00;      // no extension blocks

    UINT32 Sum = 0;
    for (int Index = 0; Index < DESKSPLIT_EDID_SIZE - 1; ++Index)
    {
        Sum += pEdid[Index];
    }
    pEdid[127] = static_cast<BYTE>((256u - (Sum & 0xFF)) & 0xFF);

    return true;
}
