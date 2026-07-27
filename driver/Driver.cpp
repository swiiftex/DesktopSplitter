// Driver.cpp - DesktopSplitterVdd
//
// WDF driver entry, IddCx client configuration, dynamic monitor management
// driven by IOCTL_DESKSPLIT_SET_CONFIG, and the swap-chain drain thread.
//
// Modelled on Microsoft's IddSampleDriver (Windows-driver-samples,
// video/IndirectDisplay, MIT licence).

#define NOMINMAX
#include <windows.h>

// NOTE: <initguid.h> is deliberately NOT included here. Storage for
// GUID_DEVINTERFACE_DESKSPLIT is emitted by Guids.cpp, the one translation unit
// that turns INITGUID on (and only after every system header has been pulled
// in). See the comment at the top of Guids.cpp.
#include "Driver.h"

#include <new>
#include <string.h>

using namespace Microsoft::WRL;
using namespace DesktopSplitter;

// ---------------------------------------------------------------------------
// WDF object context wrappers
// ---------------------------------------------------------------------------

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IndirectDeviceContextWrapper, WdfObjectGet_IndirectDeviceContextWrapper);

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IndirectMonitorContextWrapper, WdfObjectGet_IndirectMonitorContextWrapper);

// ---------------------------------------------------------------------------
// Forward declarations of the DDI callbacks
// ---------------------------------------------------------------------------

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD DeskSplitDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY DeskSplitDeviceD0Entry;

EVT_IDD_CX_DEVICE_IO_CONTROL DeskSplitIoDeviceControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED DeskSplitAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES DeskSplitAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION DeskSplitParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES DeskSplitMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES DeskSplitMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN DeskSplitMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN DeskSplitMonitorUnassignSwapChain;

// ---------------------------------------------------------------------------
// EDID lookup table
//
// EvtIddCxParseMonitorDescription is a static callback that receives only the
// monitor description bytes - no adapter or monitor handle - and the OS may
// invoke it re-entrantly from inside IddCxMonitorCreate/IddCxMonitorArrival.
// It therefore cannot take the device context lock (that would deadlock the
// arrival path). Instead every live monitor publishes its EDID and its mode
// list into this small table, guarded by its own short-lived SRW lock that is
// never held across an IddCx call.
// ---------------------------------------------------------------------------

namespace
{
    struct EdidTableEntry
    {
        bool Valid;
        BYTE Edid[DESKSPLIT_EDID_SIZE];
        DESKSPLIT_MONITOR_CONFIG Config;
    };

    EdidTableEntry g_EdidTable[DESKSPLIT_MAX_MONITORS] = {};
    SRWLOCK g_EdidTableLock = SRWLOCK_INIT;

    void PublishEdidEntry(UINT32 MonitorIndex, const BYTE* pEdid, const DESKSPLIT_MONITOR_CONFIG& Config)
    {
        if (MonitorIndex >= DESKSPLIT_MAX_MONITORS)
        {
            return;
        }

        AcquireSRWLockExclusive(&g_EdidTableLock);
        CopyMemory(g_EdidTable[MonitorIndex].Edid, pEdid, DESKSPLIT_EDID_SIZE);
        g_EdidTable[MonitorIndex].Config = Config;
        g_EdidTable[MonitorIndex].Valid = true;
        ReleaseSRWLockExclusive(&g_EdidTableLock);
    }

    void ClearEdidEntry(UINT32 MonitorIndex)
    {
        if (MonitorIndex >= DESKSPLIT_MAX_MONITORS)
        {
            return;
        }

        AcquireSRWLockExclusive(&g_EdidTableLock);
        ZeroMemory(&g_EdidTable[MonitorIndex], sizeof(g_EdidTable[MonitorIndex]));
        ReleaseSRWLockExclusive(&g_EdidTableLock);
    }

    bool LookupEdidEntry(const BYTE* pEdid, size_t Size, DESKSPLIT_MONITOR_CONFIG* pConfig)
    {
        if (pEdid == nullptr || Size != static_cast<size_t>(DESKSPLIT_EDID_SIZE) || pConfig == nullptr)
        {
            return false;
        }

        bool Found = false;

        AcquireSRWLockShared(&g_EdidTableLock);
        for (UINT32 Index = 0; Index < DESKSPLIT_MAX_MONITORS; ++Index)
        {
            if (g_EdidTable[Index].Valid &&
                memcmp(g_EdidTable[Index].Edid, pEdid, DESKSPLIT_EDID_SIZE) == 0)
            {
                *pConfig = g_EdidTable[Index].Config;
                Found = true;
                break;
            }
        }
        ReleaseSRWLockShared(&g_EdidTableLock);

        return Found;
    }

    // Deterministic per-index container id. Using a stable id (rather than a
    // freshly generated GUID on every arrival) lets Windows recognise the same
    // virtual monitor across departures/arrivals and reboots.
    GUID MakeContainerId(UINT32 MonitorIndex)
    {
        GUID ContainerId =
        {
            0x9c3d5f10, 0x71b2, 0x4a68, { 0xa5, 0x2c, 0x0b, 0x8f, 0x14, 0x6e, 0x00, 0x00 }
        };

        ContainerId.Data4[6] = static_cast<BYTE>((MonitorIndex >> 8) & 0xFF);
        ContainerId.Data4[7] = static_cast<BYTE>(MonitorIndex & 0xFF);

        return ContainerId;
    }

    // -----------------------------------------------------------------------
    // Mode helpers
    // -----------------------------------------------------------------------

    bool FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Info, const DESKSPLIT_MODE& Mode, bool bMonitorMode)
    {
        DESKSPLIT_TIMING Timing = {};
        if (!DeskSplitComputeTiming(Mode.Width, Mode.Height, Mode.RefreshMillihertz, &Timing))
        {
            return false;
        }

        Info.activeSize.cx = Timing.HActive;
        Info.activeSize.cy = Timing.VActive;
        Info.totalSize.cx = Timing.HTotal;
        Info.totalSize.cy = Timing.VTotal;

        // See DISPLAYCONFIG_VIDEO_SIGNAL_INFO. A monitor mode uses a divider of
        // 0, a target mode uses 1.
        Info.AdditionalSignalInfo.videoStandard = 255;      // DISPLAYCONFIG_VIDEO_SIGNAL_STANDARD "other"
        Info.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;

        // Fractional refresh is preserved exactly: 164999 mHz -> 164999 / 1000.
        Info.vSyncFreq.Numerator = Timing.VSyncNumerator;
        Info.vSyncFreq.Denominator = Timing.VSyncDenominator;
        Info.hSyncFreq.Numerator = Timing.HSyncNumerator;
        Info.hSyncFreq.Denominator = Timing.HSyncDenominator;

        Info.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
        Info.pixelRate = Timing.PixelClockHz;

        return true;
    }

    bool CreateIddCxMonitorMode(const DESKSPLIT_MODE& Mode, IDDCX_MONITOR_MODE_ORIGIN Origin, IDDCX_MONITOR_MODE* pOut)
    {
        IDDCX_MONITOR_MODE Result = {};
        Result.Size = sizeof(Result);
        Result.Origin = Origin;

        if (!FillSignalInfo(Result.MonitorVideoSignalInfo, Mode, true))
        {
            return false;
        }

        *pOut = Result;
        return true;
    }

    bool CreateIddCxTargetMode(const DESKSPLIT_MODE& Mode, IDDCX_TARGET_MODE* pOut)
    {
        IDDCX_TARGET_MODE Result = {};
        Result.Size = sizeof(Result);

        if (!FillSignalInfo(Result.TargetVideoSignalInfo.targetVideoSignalInfo, Mode, false))
        {
            return false;
        }

        *pOut = Result;
        return true;
    }

    // Fallback mode list, only used for a monitor that somehow has no
    // description (the driver always supplies an EDID, so this is defensive).
    const DESKSPLIT_MODE g_FallbackModes[] =
    {
        { 1920, 1080, 60000 },
        { 1280,  720, 60000 },
    };
}

// ---------------------------------------------------------------------------
// Direct3DDevice
// ---------------------------------------------------------------------------

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice()
{
    AdapterLuid = LUID{};
}

HRESULT Direct3DDevice::Init()
{
    // The DXGI factory could be cached, but if a new render adapter appears on
    // the system a new factory is needed. If caching is desired, check
    // DxgiFactory->IsCurrent() and recreate when it is no longer current.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    // Find the render adapter the OS told us the desktop was rendered on. This
    // is vendor neutral: whatever GPU Windows composited on is what we use.
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        // The render adapter is gone (detachable GPU, driver restart, ...).
        // Fall back to the system default adapter so frame draining keeps
        // working until the OS reassigns the swap-chain.
        DS_WARN("EnumAdapterByLuid failed (0x%08X), falling back to the default adapter", hr);

        hr = DxgiFactory->EnumAdapters1(0, &Adapter);
        if (FAILED(hr))
        {
            return hr;
        }
    }

    // Create a D3D device on the render adapter. BGRA support is required by
    // the WHQL test suite.
    hr = D3D11CreateDevice(
        Adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &Device,
        nullptr,
        &DeviceContext);
    if (FAILED(hr))
    {
        // If creating the D3D device failed the render GPU may have been lost
        // or the system is in a transient state.
        return hr;
    }

    return S_OK;
}

// ---------------------------------------------------------------------------
// SwapChainProcessor
// ---------------------------------------------------------------------------

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
    : m_hSwapChain(hSwapChain)
    , m_Device(Device)
    , m_hAvailableBufferEvent(NewFrameEvent)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

    // Immediately create and run the swap-chain processing thread, passing
    // 'this' as the thread parameter.
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    // Alert the swap-chain processing thread to terminate.
    SetEvent(m_hTerminateEvent.Get());

    if (m_hThread.Get())
    {
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    // Use the Multimedia Class Scheduler Service so this thread is prioritised
    // for throughput under high CPU load.
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

    RunCore();

    // Always delete the swap-chain object when the processing loop terminates,
    // to kick the system into providing a new swap-chain if necessary.
    WdfObjectDelete(reinterpret_cast<WDFOBJECT>(m_hSwapChain));
    m_hSwapChain = nullptr;

    if (AvTaskHandle != nullptr)
    {
        AvRevertMmThreadCharacteristics(AvTaskHandle);
    }
}

void SwapChainProcessor::RunCore()
{
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr))
    {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        return;
    }

    // Acquire and release buffers in a loop. DesktopSplitter does no encoding
    // and no copy-out here: the frames the user actually sees are captured from
    // the virtual monitors by the compositor via DXGI desktop duplication. The
    // driver only has to keep the swap-chain draining so the OS keeps
    // presenting to it.
    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;

        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        if (hr == E_PENDING)
        {
            // No buffer available yet; wait for one.
            HANDLE WaitHandles[] =
            {
                m_hAvailableBufferEvent,
                m_hTerminateEvent.Get()
            };

            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
            if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
            {
                continue;
            }
            else if (WaitResult == WAIT_OBJECT_0 + 1)
            {
                break;
            }
            else
            {
                break;
            }
        }
        else if (SUCCEEDED(hr))
        {
            // The driver owns a reference on the surface and must release it.
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);

            // No processing is performed on the frame.
            AcquiredBuffer.Reset();

            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                break;
            }
        }
        else
        {
            // The swap-chain was abandoned (e.g. DXGI_ERROR_ACCESS_LOST).
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// IndirectMonitorContext
// ---------------------------------------------------------------------------

IndirectMonitorContext::IndirectMonitorContext(IDDCX_MONITOR Monitor, UINT32 MonitorIndex, const DESKSPLIT_MONITOR_CONFIG& Config)
    : m_Monitor(Monitor)
    , m_MonitorIndex(MonitorIndex)
    , m_Config(Config)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();

    auto Device = std::make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init()))
    {
        // The swap-chain cannot be processed. Delete it; the OS will provide a
        // new one when it is able to.
        DS_ERROR("Failed to create a D3D device for the assigned swap-chain");
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(SwapChain));
        return;
    }

    m_ProcessingThread.reset(new (std::nothrow) SwapChainProcessor(SwapChain, Device, NewFrameEvent));
    if (!m_ProcessingThread)
    {
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(SwapChain));
    }
}

void IndirectMonitorContext::UnassignSwapChain()
{
    // Stop the processing thread; its destructor waits for the thread to exit.
    m_ProcessingThread.reset();
}

// ---------------------------------------------------------------------------
// IndirectDeviceContext
// ---------------------------------------------------------------------------

IndirectDeviceContext::IndirectDeviceContext(WDFDEVICE WdfDevice)
    : m_WdfDevice(WdfDevice)
    , m_Adapter(nullptr)
    , m_AdapterInitStarted(false)
    , m_AdapterReady(false)
    , m_HasPendingConfig(false)
{
    ZeroMemory(&m_PendingConfig, sizeof(m_PendingConfig));
    ZeroMemory(m_Slots, sizeof(m_Slots));
}

IndirectDeviceContext::~IndirectDeviceContext()
{
    for (UINT32 Index = 0; Index < DESKSPLIT_MAX_MONITORS; ++Index)
    {
        ClearEdidEntry(Index);
    }
}

void IndirectDeviceContext::InitAdapter()
{
    std::lock_guard<std::recursive_mutex> Lock(m_Lock);

    if (m_AdapterInitStarted)
    {
        // D0Entry can run more than once across power transitions; the adapter
        // is initialised exactly once.
        return;
    }

    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);
    AdapterCaps.MaxMonitorsSupported = DESKSPLIT_MAX_MONITORS;

    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"DesktopSplitter Virtual Display";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"DesktopSplitter";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"DesktopSplitter VDD";

    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    // Context on the adapter object so the static DDI callbacks can reach this
    // object. No cleanup callback: the adapter context is an alias of the
    // device context, which owns the lifetime.
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    IDARG_OUT_ADAPTER_INIT AdapterInitOut = {};
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);
    if (!NT_SUCCESS(Status))
    {
        DS_ERROR("IddCxAdapterInitAsync failed: 0x%08X", Status);
        return;
    }

    m_AdapterInitStarted = true;
    m_Adapter = AdapterInitOut.AdapterObject;

    auto* pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
    pWrapper->pContext = this;
}

_Use_decl_annotations_
void IndirectDeviceContext::OnAdapterInitFinished(IDDCX_ADAPTER AdapterObject, NTSTATUS InitStatus)
{
    std::lock_guard<std::recursive_mutex> Lock(m_Lock);

    m_Adapter = AdapterObject;

    if (!NT_SUCCESS(InitStatus))
    {
        DS_ERROR("Adapter initialisation failed: 0x%08X", InitStatus);
        m_AdapterReady = false;
        return;
    }

    m_AdapterReady = true;

    DESKSPLIT_CONFIG Config = {};

    if (m_HasPendingConfig)
    {
        Config = m_PendingConfig;
        m_HasPendingConfig = false;
        ZeroMemory(&m_PendingConfig, sizeof(m_PendingConfig));
    }
    else
    {
        NTSTATUS Status = LoadPersistedConfig(&Config);
        if (!NT_SUCCESS(Status))
        {
            // First install, or the stored blob is missing/corrupt: start with
            // zero virtual monitors, exactly as the architecture requires.
            DS_INFO("No usable persisted configuration (0x%08X); starting with 0 monitors", Status);
            return;
        }
    }

    DS_INFO("Replaying configuration with %u monitor(s)", Config.MonitorCount);
    (void)ApplyConfig(Config, false);
}

_Use_decl_annotations_
NTSTATUS IndirectDeviceContext::ApplyConfig(const DESKSPLIT_CONFIG& Config, bool Persist)
{
    if (!DeskSplitIsConfigValid(Config))
    {
        return STATUS_INVALID_PARAMETER;
    }

    std::lock_guard<std::recursive_mutex> Lock(m_Lock);

    if (Persist)
    {
        // Persistence is best effort: an unwritable software key must not stop
        // the monitors from being created.
        NTSTATUS PersistStatus = PersistConfig(Config);
        if (!NT_SUCCESS(PersistStatus))
        {
            DS_WARN("Failed to persist configuration: 0x%08X", PersistStatus);
        }
    }

    if (!m_AdapterReady)
    {
        // Config arrived before EvtIddCxAdapterInitFinished; replay it there.
        m_PendingConfig = Config;
        m_HasPendingConfig = true;
        DS_INFO("Adapter not ready yet; configuration queued");
        return STATUS_SUCCESS;
    }

    // Pass 1: depart monitors that are no longer wanted, or whose mode list
    // changed (a changed mode list means a new EDID, so the monitor has to be
    // re-plugged for Windows to pick it up).
    for (UINT32 Index = 0; Index < DESKSPLIT_MAX_MONITORS; ++Index)
    {
        if (!m_Slots[Index].Active)
        {
            continue;
        }

        const bool Wanted = (Index < Config.MonitorCount);
        const bool Same = Wanted &&
            (memcmp(&m_Slots[Index].Config, &Config.Monitors[Index], sizeof(DESKSPLIT_MONITOR_CONFIG)) == 0);

        if (!Same)
        {
            NTSTATUS Status = RemoveMonitorLocked(Index);
            if (!NT_SUCCESS(Status))
            {
                DS_WARN("IddCxMonitorDeparture for monitor %u failed: 0x%08X", Index, Status);
            }
        }
    }

    // Pass 2: create/arrive the monitors that are missing.
    NTSTATUS Result = STATUS_SUCCESS;
    for (UINT32 Index = 0; Index < Config.MonitorCount; ++Index)
    {
        if (m_Slots[Index].Active)
        {
            continue;
        }

        NTSTATUS Status = CreateMonitorLocked(Index, Config.Monitors[Index]);
        if (!NT_SUCCESS(Status))
        {
            DS_ERROR("Failed to create monitor %u: 0x%08X", Index, Status);
            Result = Status;
        }
    }

    return Result;
}

_Use_decl_annotations_
void IndirectDeviceContext::GetStatus(DESKSPLIT_STATUS* pStatus)
{
    std::lock_guard<std::recursive_mutex> Lock(m_Lock);

    pStatus->Version = DESKSPLIT_PROTOCOL_VERSION;

    UINT32 ActiveCount = 0;
    for (UINT32 Index = 0; Index < DESKSPLIT_MAX_MONITORS; ++Index)
    {
        if (m_Slots[Index].Active)
        {
            ++ActiveCount;
        }
    }

    pStatus->ActiveMonitorCount = ActiveCount;
}

_Use_decl_annotations_
NTSTATUS IndirectDeviceContext::CreateMonitorLocked(UINT32 MonitorIndex, const DESKSPLIT_MONITOR_CONFIG& Config)
{
    if (MonitorIndex >= DESKSPLIT_MAX_MONITORS)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (m_Adapter == nullptr)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    MonitorSlot& Slot = m_Slots[MonitorIndex];

    if (!DeskSplitBuildEdid(MonitorIndex, Config, Slot.Edid))
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Publish before the create call: the OS parses the description (possibly
    // re-entrantly) during IddCxMonitorCreate/IddCxMonitorArrival.
    PublishEdidEntry(MonitorIndex, Slot.Edid, Config);

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(Object);
        if (pWrapper != nullptr)
        {
            pWrapper->Cleanup();
        }
    };

    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    MonitorInfo.ConnectorIndex = MonitorIndex;
    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = DESKSPLIT_EDID_SIZE;
    MonitorInfo.MonitorDescription.pData = Slot.Edid;
    MonitorInfo.MonitorContainerId = MakeContainerId(MonitorIndex);

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    IDARG_OUT_MONITORCREATE MonitorCreateOut = {};
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (!NT_SUCCESS(Status))
    {
        ClearEdidEntry(MonitorIndex);
        return Status;
    }

    auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
    pWrapper->pContext = new (std::nothrow) IndirectMonitorContext(MonitorCreateOut.MonitorObject, MonitorIndex, Config);
    if (pWrapper->pContext == nullptr)
    {
        (void)IddCxMonitorDeparture(MonitorCreateOut.MonitorObject);
        ClearEdidEntry(MonitorIndex);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // Tell the OS the monitor has been plugged in.
    IDARG_OUT_MONITORARRIVAL ArrivalOut = {};
    Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
    if (!NT_SUCCESS(Status))
    {
        (void)IddCxMonitorDeparture(MonitorCreateOut.MonitorObject);
        ClearEdidEntry(MonitorIndex);
        return Status;
    }

    Slot.Active = true;
    Slot.Monitor = MonitorCreateOut.MonitorObject;
    Slot.Config = Config;

    DS_INFO("Monitor %u arrived (%ux%u @ %u mHz, %u mode(s))",
        MonitorIndex,
        Config.Modes[Config.PreferredModeIndex].Width,
        Config.Modes[Config.PreferredModeIndex].Height,
        Config.Modes[Config.PreferredModeIndex].RefreshMillihertz,
        Config.ModeCount);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IndirectDeviceContext::RemoveMonitorLocked(UINT32 MonitorIndex)
{
    if (MonitorIndex >= DESKSPLIT_MAX_MONITORS)
    {
        return STATUS_INVALID_PARAMETER;
    }

    MonitorSlot& Slot = m_Slots[MonitorIndex];
    if (!Slot.Active)
    {
        return STATUS_SUCCESS;
    }

    IDDCX_MONITOR Monitor = Slot.Monitor;

    Slot.Active = false;
    Slot.Monitor = nullptr;
    ZeroMemory(&Slot.Config, sizeof(Slot.Config));
    ZeroMemory(Slot.Edid, sizeof(Slot.Edid));

    // IddCx destroys the IDDCX_MONITOR object as part of the departure; the
    // object context cleanup callback deletes our IndirectMonitorContext (which
    // stops the swap-chain processing thread). The EDID entry stays published
    // until the call returns, in case the OS parses the description one last
    // time on the way out.
    NTSTATUS Status = IddCxMonitorDeparture(Monitor);

    ClearEdidEntry(MonitorIndex);

    DS_INFO("Monitor %u departed (0x%08X)", MonitorIndex, Status);

    return Status;
}

_Use_decl_annotations_
NTSTATUS IndirectDeviceContext::LoadPersistedConfig(DESKSPLIT_CONFIG* pConfig)
{
    if (pConfig == nullptr)
    {
        return STATUS_INVALID_PARAMETER;
    }

    ZeroMemory(pConfig, sizeof(*pConfig));

    WDFKEY hKey = nullptr;
    NTSTATUS Status = WdfDeviceOpenRegistryKey(
        m_WdfDevice,
        PLUGPLAY_REGKEY_DRIVER,
        KEY_QUERY_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &hKey);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    DECLARE_CONST_UNICODE_STRING(ValueName, DESKSPLIT_REG_CONFIG_VALUE);

    ULONG ValueLength = 0;
    ULONG ValueType = 0;
    Status = WdfRegistryQueryValue(
        hKey,
        &ValueName,
        sizeof(*pConfig),
        pConfig,
        &ValueLength,
        &ValueType);

    WdfRegistryClose(hKey);

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }
    if (ValueType != REG_BINARY || ValueLength != sizeof(*pConfig))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (!DeskSplitIsConfigValid(*pConfig))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS IndirectDeviceContext::PersistConfig(const DESKSPLIT_CONFIG& Config)
{
    WDFKEY hKey = nullptr;
    NTSTATUS Status = WdfDeviceOpenRegistryKey(
        m_WdfDevice,
        PLUGPLAY_REGKEY_DRIVER,
        KEY_SET_VALUE,
        WDF_NO_OBJECT_ATTRIBUTES,
        &hKey);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    DECLARE_CONST_UNICODE_STRING(ValueName, DESKSPLIT_REG_CONFIG_VALUE);

    Status = WdfRegistryAssignValue(
        hKey,
        &ValueName,
        REG_BINARY,
        sizeof(Config),
        const_cast<DESKSPLIT_CONFIG*>(&Config));

    WdfRegistryClose(hKey);

    return Status;
}

// ---------------------------------------------------------------------------
// WDF entry points
// ---------------------------------------------------------------------------

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDF_OBJECT_ATTRIBUTES Attributes;

    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
    WDF_DRIVER_CONFIG_INIT(&Config, DeskSplitDeviceAdd);

    NTSTATUS Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status))
    {
        DS_ERROR("WdfDriverCreate failed: 0x%08X", Status);
    }

    return Status;
}

#if DESKSPLIT_USE_WDF_DEFAULT_QUEUE

// Optional: a WDF default queue for the control IOCTLs.
//
// This is OFF by default and should stay that way. IddCx redirects
// IoDeviceControl requests to an internal queue, so a driver-owned default
// queue never sees them - EvtIddCxDeviceIoControl is the supported way for an
// IddCx driver to handle custom IOCTLs (see the comment in Microsoft's
// IddSampleDriver). The queue below is provided only for the case where a
// future IddCx version stops redirecting; both paths funnel into the same
// handler, so behaviour is identical either way.
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL DeskSplitQueueIoDeviceControl;

_Use_decl_annotations_
VOID DeskSplitQueueIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    DeskSplitIoDeviceControl(
        WdfIoQueueGetDevice(Queue),
        Request,
        OutputBufferLength,
        InputBufferLength,
        IoControlCode);
}

static NTSTATUS DeskSplitCreateDefaultQueue(WDFDEVICE Device)
{
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoDeviceControl = DeskSplitQueueIoDeviceControl;

    WDFQUEUE Queue = nullptr;
    return WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &Queue);
}

#endif // DESKSPLIT_USE_WDF_DEFAULT_QUEUE

_Use_decl_annotations_
NTSTATUS DeskSplitDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    // Power callbacks: the adapter is initialised on the way to D0.
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = DeskSplitDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    // IddCx redirects IoDeviceControl requests to an internal queue, so custom
    // control IOCTLs must be handled through this callback.
    IddConfig.EvtIddCxDeviceIoControl = DeskSplitIoDeviceControl;

    IddConfig.EvtIddCxAdapterInitFinished = DeskSplitAdapterInitFinished;
    IddConfig.EvtIddCxParseMonitorDescription = DeskSplitParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = DeskSplitMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = DeskSplitMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = DeskSplitAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = DeskSplitMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = DeskSplitMonitorUnassignSwapChain;

    NTSTATUS Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status))
    {
        DS_ERROR("IddCxDeviceInitConfig failed: 0x%08X", Status);
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object)
    {
        // Automatically clean up the context when the WDF object is deleted.
        auto* pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pWrapper != nullptr)
        {
            pWrapper->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status))
    {
        DS_ERROR("WdfDeviceCreate failed: 0x%08X", Status);
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);
    if (!NT_SUCCESS(Status))
    {
        DS_ERROR("IddCxDeviceInitialize failed: 0x%08X", Status);
        return Status;
    }

    // The control app finds and opens the driver through this interface.
    Status = WdfDeviceCreateDeviceInterface(Device, &GUID_DEVINTERFACE_DESKSPLIT, nullptr);
    if (!NT_SUCCESS(Status))
    {
        DS_ERROR("WdfDeviceCreateDeviceInterface failed: 0x%08X", Status);
        return Status;
    }

#if DESKSPLIT_USE_WDF_DEFAULT_QUEUE
    NTSTATUS QueueStatus = DeskSplitCreateDefaultQueue(Device);
    if (!NT_SUCCESS(QueueStatus))
    {
        // Not fatal: EvtIddCxDeviceIoControl still delivers the IOCTLs.
        DS_WARN("Default IO queue creation failed: 0x%08X", QueueStatus);
    }
#endif

    auto* pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pWrapper->pContext = new (std::nothrow) IndirectDeviceContext(Device);
    if (pWrapper->pContext == nullptr)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DeskSplitDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    auto* pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    if (pWrapper == nullptr || pWrapper->pContext == nullptr)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    pWrapper->pContext->InitAdapter();

    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Control IOCTLs
// ---------------------------------------------------------------------------

_Use_decl_annotations_
VOID DeskSplitIoDeviceControl(
    WDFDEVICE Device,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Information = 0;

    auto* pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    IndirectDeviceContext* pContext = (pWrapper != nullptr) ? pWrapper->pContext : nullptr;

    switch (IoControlCode)
    {
    case IOCTL_DESKSPLIT_SET_CONFIG:
    {
        if (InputBufferLength < sizeof(DESKSPLIT_CONFIG))
        {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        PVOID pBuffer = nullptr;
        size_t BufferLength = 0;
        Status = WdfRequestRetrieveInputBuffer(Request, sizeof(DESKSPLIT_CONFIG), &pBuffer, &BufferLength);
        if (!NT_SUCCESS(Status))
        {
            break;
        }

        // WdfRequestRetrieveInputBuffer already guarantees the minimum length,
        // but re-check explicitly: it keeps the contract local to the copy and
        // satisfies static analysis.
        if (pBuffer == nullptr || BufferLength < sizeof(DESKSPLIT_CONFIG))
        {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        // Copy out of the request buffer before validating, so the contents
        // cannot change underneath the validation.
        DESKSPLIT_CONFIG Config;
        CopyMemory(&Config, pBuffer, sizeof(Config));

        if (!DeskSplitIsConfigValid(Config))
        {
            DS_WARN("Rejecting invalid DESKSPLIT_CONFIG (version %u, %u monitors)",
                Config.Version, Config.MonitorCount);
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (pContext == nullptr)
        {
            Status = STATUS_DEVICE_NOT_READY;
            break;
        }

        Status = pContext->ApplyConfig(Config, true);
        break;
    }

    case IOCTL_DESKSPLIT_GET_STATUS:
    {
        if (OutputBufferLength < sizeof(DESKSPLIT_STATUS))
        {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        PVOID pBuffer = nullptr;
        size_t BufferLength = 0;
        Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(DESKSPLIT_STATUS), &pBuffer, &BufferLength);
        if (!NT_SUCCESS(Status))
        {
            break;
        }

        if (pBuffer == nullptr || BufferLength < sizeof(DESKSPLIT_STATUS))
        {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        DESKSPLIT_STATUS StatusOut = {};
        StatusOut.Version = DESKSPLIT_PROTOCOL_VERSION;
        StatusOut.ActiveMonitorCount = 0;

        if (pContext != nullptr)
        {
            pContext->GetStatus(&StatusOut);
        }

        CopyMemory(pBuffer, &StatusOut, sizeof(StatusOut));
        Information = sizeof(StatusOut);
        Status = STATUS_SUCCESS;
        break;
    }

    default:
        break;
    }

    WdfRequestCompleteWithInformation(Request, Status, Information);
}

// ---------------------------------------------------------------------------
// IddCx DDI callbacks
// ---------------------------------------------------------------------------

_Use_decl_annotations_
NTSTATUS DeskSplitAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    // The OS has finished setting up the adapter; monitors may now be reported.
    auto* pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    if (pWrapper == nullptr || pWrapper->pContext == nullptr)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    pWrapper->pContext->OnAdapterInitFinished(AdapterObject, pInArgs->AdapterInitStatus);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DeskSplitAdapterCommitModes(IDDCX_ADAPTER AdapterObject, const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    // Nothing to reconfigure: there is no real transmitter behind these
    // monitors. IddCx takes care of the swap-chains.
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DeskSplitParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs,
    IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    if (pInArgs->MonitorDescription.DataSize != static_cast<UINT>(DESKSPLIT_EDID_SIZE))
    {
        return STATUS_INVALID_PARAMETER;
    }

    DESKSPLIT_MONITOR_CONFIG Config = {};
    if (!LookupEdidEntry(
            reinterpret_cast<const BYTE*>(pInArgs->MonitorDescription.pData),
            pInArgs->MonitorDescription.DataSize,
            &Config))
    {
        // Not one of the EDIDs this driver published.
        return STATUS_INVALID_PARAMETER;
    }

    pOutArgs->MonitorModeBufferOutputCount = Config.ModeCount;

    if (pInArgs->MonitorModeBufferInputCount < Config.ModeCount)
    {
        // A zero input count means the caller only wants the mode count.
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }

    for (UINT32 ModeIndex = 0; ModeIndex < Config.ModeCount; ++ModeIndex)
    {
        if (!CreateIddCxMonitorMode(
                Config.Modes[ModeIndex],
                IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR,
                &pInArgs->pMonitorModes[ModeIndex]))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    pOutArgs->PreferredMonitorModeIdx = Config.PreferredModeIndex;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DeskSplitMonitorGetDefaultModes(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    // Only reached for a monitor with no description. This driver always
    // supplies an EDID, so this path is defensive; report the monitor's
    // configured modes when they are known and a safe default list otherwise.
    const DESKSPLIT_MODE* pModes = g_FallbackModes;
    UINT32 ModeCount = static_cast<UINT32>(ARRAYSIZE(g_FallbackModes));
    UINT32 PreferredIndex = 0;

    auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    if (pWrapper != nullptr && pWrapper->pContext != nullptr)
    {
        const DESKSPLIT_MONITOR_CONFIG& Config = pWrapper->pContext->Config();
        if (Config.ModeCount > 0 && Config.ModeCount <= DESKSPLIT_MAX_MODES)
        {
            pModes = Config.Modes;
            ModeCount = Config.ModeCount;
            PreferredIndex = Config.PreferredModeIndex;
        }
    }

    pOutArgs->DefaultMonitorModeBufferOutputCount = ModeCount;

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0 || pInArgs->pDefaultMonitorModes == nullptr)
    {
        return STATUS_SUCCESS;
    }
    if (pInArgs->DefaultMonitorModeBufferInputCount < ModeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (UINT32 ModeIndex = 0; ModeIndex < ModeCount; ++ModeIndex)
    {
        if (!CreateIddCxMonitorMode(
                pModes[ModeIndex],
                IDDCX_MONITOR_MODE_ORIGIN_DRIVER,
                &pInArgs->pDefaultMonitorModes[ModeIndex]))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    pOutArgs->PreferredMonitorModeIdx = PreferredIndex;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DeskSplitMonitorQueryModes(
    IDDCX_MONITOR MonitorObject,
    const IDARG_IN_QUERYTARGETMODES* pInArgs,
    IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    // The set of modes this "device" can scan out. Windows shows the
    // intersection of the monitor modes with these, so reporting exactly the
    // configured list makes each virtual monitor advertise exactly its
    // configured modes and nothing else.
    DESKSPLIT_MONITOR_CONFIG Config = {};
    bool HaveConfig = false;

    auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    if (pWrapper != nullptr && pWrapper->pContext != nullptr)
    {
        Config = pWrapper->pContext->Config();
        HaveConfig = (Config.ModeCount > 0 && Config.ModeCount <= DESKSPLIT_MAX_MODES);
    }

    if (!HaveConfig)
    {
        // Fall back to the description the OS handed us.
        HaveConfig = LookupEdidEntry(
            reinterpret_cast<const BYTE*>(pInArgs->MonitorDescription.pData),
            pInArgs->MonitorDescription.DataSize,
            &Config);
    }

    if (!HaveConfig)
    {
        ZeroMemory(&Config, sizeof(Config));
        Config.ModeCount = static_cast<UINT32>(ARRAYSIZE(g_FallbackModes));
        Config.PreferredModeIndex = 0;
        for (UINT32 ModeIndex = 0; ModeIndex < Config.ModeCount; ++ModeIndex)
        {
            Config.Modes[ModeIndex] = g_FallbackModes[ModeIndex];
        }
    }

    pOutArgs->TargetModeBufferOutputCount = Config.ModeCount;

    if (pInArgs->TargetModeBufferInputCount == 0 || pInArgs->pTargetModes == nullptr)
    {
        return STATUS_SUCCESS;
    }
    if (pInArgs->TargetModeBufferInputCount < Config.ModeCount)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (UINT32 ModeIndex = 0; ModeIndex < Config.ModeCount; ++ModeIndex)
    {
        if (!CreateIddCxTargetMode(Config.Modes[ModeIndex], &pInArgs->pTargetModes[ModeIndex]))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DeskSplitMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    if (pWrapper == nullptr || pWrapper->pContext == nullptr)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    pWrapper->pContext->AssignSwapChain(
        pInArgs->hSwapChain,
        pInArgs->RenderAdapterLuid,
        pInArgs->hNextSurfaceAvailable);

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS DeskSplitMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    if (pWrapper == nullptr || pWrapper->pContext == nullptr)
    {
        return STATUS_SUCCESS;
    }

    pWrapper->pContext->UnassignSwapChain();

    return STATUS_SUCCESS;
}
