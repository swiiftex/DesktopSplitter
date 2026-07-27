// Driver.h - DesktopSplitterVdd, an IddCx (>= 1.4) UMDF 2 indirect display
// driver that publishes 0..DESKSPLIT_MAX_MONITORS virtual monitors under
// runtime control of the DesktopSplitter control app.
//
// Structure follows Microsoft's IddSampleDriver (Windows-driver-samples,
// video/IndirectDisplay, MIT licence).
#pragma once

#define NOMINMAX

#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <mutex>
#include <vector>

#include "Trace.h"
#include "Timing.h"
#include "Edid.h"

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes.
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

namespace DesktopSplitter
{
    /// <summary>
    /// Manages the creation and lifetime of a Direct3D render device.
    /// </summary>
    struct Direct3DDevice
    {
        explicit Direct3DDevice(LUID AdapterLuid);
        Direct3DDevice();

        // Creates a D3D11 device on the adapter identified by AdapterLuid. If
        // that adapter cannot be found (e.g. it was removed) the system default
        // adapter is used instead. No vendor specific APIs are involved.
        HRESULT Init();

        LUID AdapterLuid;
        Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
        Microsoft::WRL::ComPtr<ID3D11Device> Device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
    };

    /// <summary>
    /// Manages a thread that consumes buffers from an indirect display swap-chain object.
    /// </summary>
    class SwapChainProcessor
    {
    public:
        SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
        ~SwapChainProcessor();

    private:
        static DWORD CALLBACK RunThread(LPVOID Argument);

        void Run();
        void RunCore();

        IDDCX_SWAPCHAIN m_hSwapChain;
        std::shared_ptr<Direct3DDevice> m_Device;
        HANDLE m_hAvailableBufferEvent;
        Microsoft::WRL::Wrappers::Thread m_hThread;
        Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
    };

    /// <summary>
    /// Per-monitor state. Owned by the IDDCX_MONITOR object context.
    /// </summary>
    class IndirectMonitorContext
    {
    public:
        IndirectMonitorContext(IDDCX_MONITOR Monitor, UINT32 MonitorIndex, const DESKSPLIT_MONITOR_CONFIG& Config);
        virtual ~IndirectMonitorContext();

        void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
        void UnassignSwapChain();

        UINT32 Index() const { return m_MonitorIndex; }
        const DESKSPLIT_MONITOR_CONFIG& Config() const { return m_Config; }

    private:
        IDDCX_MONITOR m_Monitor;
        UINT32 m_MonitorIndex;
        DESKSPLIT_MONITOR_CONFIG m_Config;
        std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
    };

    /// <summary>
    /// Per-adapter state. Owned by the WDFDEVICE object context.
    /// </summary>
    class IndirectDeviceContext
    {
    public:
        explicit IndirectDeviceContext(WDFDEVICE WdfDevice);
        virtual ~IndirectDeviceContext();

        // Kicks off IddCxAdapterInitAsync. Safe to call more than once; only
        // the first call does anything.
        void InitAdapter();

        // Called from EvtIddCxAdapterInitFinished. Replays the persisted (or
        // pending) configuration.
        void OnAdapterInitFinished(_In_ IDDCX_ADAPTER AdapterObject, _In_ NTSTATUS InitStatus);

        // IOCTL_DESKSPLIT_SET_CONFIG. Validates, diffs against the live state,
        // departs removed monitors, creates/arrives new ones and persists the
        // blob to the driver software key.
        NTSTATUS ApplyConfig(_In_ const DESKSPLIT_CONFIG& Config, _In_ bool Persist);

        // IOCTL_DESKSPLIT_GET_STATUS.
        void GetStatus(_Out_ DESKSPLIT_STATUS* pStatus);

    private:
        NTSTATUS CreateMonitorLocked(_In_ UINT32 MonitorIndex, _In_ const DESKSPLIT_MONITOR_CONFIG& Config);
        NTSTATUS RemoveMonitorLocked(_In_ UINT32 MonitorIndex);

        NTSTATUS LoadPersistedConfig(_Out_ DESKSPLIT_CONFIG* pConfig);
        NTSTATUS PersistConfig(_In_ const DESKSPLIT_CONFIG& Config);

        struct MonitorSlot
        {
            bool Active;
            IDDCX_MONITOR Monitor;
            DESKSPLIT_MONITOR_CONFIG Config;
            BYTE Edid[DESKSPLIT_EDID_SIZE];
        };

        WDFDEVICE m_WdfDevice;
        IDDCX_ADAPTER m_Adapter;

        bool m_AdapterInitStarted;
        bool m_AdapterReady;

        bool m_HasPendingConfig;
        DESKSPLIT_CONFIG m_PendingConfig;

        MonitorSlot m_Slots[DESKSPLIT_MAX_MONITORS];

        // Held across the whole apply operation, i.e. across IddCx calls.
        // EvtIddCxParseMonitorDescription deliberately does NOT take this lock
        // (see the EDID lookup table in Driver.cpp) because the OS may call it
        // re-entrantly from inside IddCxMonitorArrival.
        std::recursive_mutex m_Lock;
    };
}
