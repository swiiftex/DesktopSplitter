#include "SpecializedPresenter.h"
#include "Log.h"

// C++/WinRT and the SDK projections are not /W4-clean and this project builds
// /W4 /WX, so warnings are suppressed for these headers only.
#pragma warning(push, 0)
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Display.h>
#include <winrt/Windows.Devices.Display.Core.h>
#include <winrt/Windows.Graphics.h>
#include <windows.devices.display.core.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#pragma warning(pop)

#include <d3d11_4.h>
#include <sddl.h>
#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace wdd = winrt::Windows::Devices::Display;
namespace wddc = winrt::Windows::Devices::Display::Core;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace ds {

namespace {

constexpr uint32_t kPrimaryCount = 2;   // double-buffered scanout

// Consecutive failed scanouts tolerated before the display is declared lost.
// A transient hiccup around a mode change should not kill the compositor, but
// a display that has genuinely stopped accepting frames must not be limped on.
constexpr unsigned kMaxConsecutiveScanoutFailures = 60;

std::wstring LowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

// Both forms name the same devnode but differ in prefix and case.
std::wstring DevnodeKey(const std::wstring& path) {
    const std::wstring low = LowerCopy(path);
    const size_t p = low.find(L"display#");
    return (p == std::wstring::npos) ? low : low.substr(p);
}

constexpr DWORD kWaitPollMs = 400;          // how often to retry acquisition
constexpr DWORD kWaitLogEveryMs = 2000;     // how often to print the countdown

// DisplayConfig device-info type 12. Ships in wingdi.h but is left out of the
// documented constants table; see compositor/README.md for the bit meanings.
constexpr int kGetMonitorSpecialization = 12;

struct SpecializationBits {
    bool valid = false;
    bool enabled = false;
    bool availableForMonitor = false;
    bool availableForSystem = false;
};

typedef struct _DS_SPECIALIZATION {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    union {
        struct {
            UINT32 isEnabled : 1;
            UINT32 availableForMonitor : 1;
            UINT32 availableForSystem : 1;
            UINT32 reserved : 29;
        } bits;
        UINT32 value;
    };
} DS_SPECIALIZATION;

// Best-effort read of the specialization state for a monitor identified by its
// device interface path. Purely informational - it makes the waiting log say
// "still on the desktop" instead of a bare retry count.
SpecializationBits QuerySpecialization(const std::wstring& interfacePath) {
    SpecializationBits out;
    if (interfacePath.empty()) return out;

    UINT32 pathCount = 0, modeCount = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) !=
        ERROR_SUCCESS) {
        return out;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (::QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(), &modeCount,
                             modes.data(), nullptr) != ERROR_SUCCESS) {
        return out;
    }
    paths.resize(pathCount);

    const std::wstring want = DevnodeKey(interfacePath);
    for (const auto& path : paths) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME name = {};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = path.targetInfo.adapterId;
        name.header.id = path.targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) continue;
        if (DevnodeKey(name.monitorDevicePath) != want) continue;

        DS_SPECIALIZATION spec = {};
        spec.header.type =
            static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(kGetMonitorSpecialization);
        spec.header.size = sizeof(spec);
        spec.header.adapterId = path.targetInfo.adapterId;
        spec.header.id = path.targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&spec.header) != ERROR_SUCCESS) return out;

        out.valid = true;
        out.enabled = spec.bits.isEnabled != 0;
        out.availableForMonitor = spec.bits.availableForMonitor != 0;
        out.availableForSystem = spec.bits.availableForSystem != 0;
        return out;
    }
    return out;
}

// Signalled once a composite frame has actually been scanned out to the hidden
// display. The control app treats "signalled within N seconds of launch" as
// proof the display is really showing content, and keeps its specialization.
const wchar_t* const kLiveEventName = L"DeskSplitSpecializedLive";

// Authenticated Users get SYNCHRONIZE only: the app waits on this, it never
// sets it. Admins/SYSTEM keep full control.
const wchar_t* const kLiveEventSddl =
    L"D:P(A;;0x1f0003;;;BA)(A;;0x1f0003;;;SY)(A;;0x00100000;;;AU)";

// Creates the liveness event, preferring the Global namespace. An unelevated
// dscomp has no SeCreateGlobalPrivilege, so fall back to the session-local
// namespace rather than running without a signal - and say which was used.
HANDLE CreateLivenessEvent() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kLiveEventSddl, SDDL_REVISION_1, &sd, nullptr)) {
        DS_WARN(L"specialized: could not build the liveness event DACL (0x%08X)",
                ::GetLastError());
        return nullptr;
    }
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;

    std::wstring full = std::wstring(L"Global\\") + kLiveEventName;
    HANDLE h = ::CreateEventW(&sa, TRUE, FALSE, full.c_str());
    if (!h && ::GetLastError() == ERROR_ACCESS_DENIED) {
        // Either no Global privilege, or the object exists and CreateEvent's
        // implicit EVENT_ALL_ACCESS request was refused. Try to open it with
        // just what we need before changing namespace.
        h = ::OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, full.c_str());
        if (!h) {
            full = std::wstring(L"Local\\") + kLiveEventName;
            h = ::CreateEventW(&sa, TRUE, FALSE, full.c_str());
        }
    }
    ::LocalFree(sd);

    if (h) {
        // Fresh per compositor run: a stale signal from a previous run must not
        // be mistaken for this one succeeding.
        ::ResetEvent(h);
        DS_LOG(L"specialized: liveness event ready: %s", full.c_str());
    } else {
        DS_WARN(L"specialized: could not create the liveness event (0x%08X) - the "
                L"control app will not see a live signal",
                ::GetLastError());
    }
    return h;
}

const wchar_t* PresentStatusName(wddc::DisplayPresentStatus s2) {
    switch (s2) {
    case wddc::DisplayPresentStatus::Success:                     return L"Success";
    case wddc::DisplayPresentStatus::SourceStatusPreventedPresent: return L"SourceStatusPreventedPresent";
    case wddc::DisplayPresentStatus::ScanoutInvalid:              return L"ScanoutInvalid";
    case wddc::DisplayPresentStatus::SourceInvalid:               return L"SourceInvalid";
    case wddc::DisplayPresentStatus::DeviceInvalid:               return L"DeviceInvalid";
    default:                                                      return L"UnknownFailure";
    }
}

const wchar_t* SourceStatusName(wddc::DisplaySourceStatus s2) {
    switch (s2) {
    case wddc::DisplaySourceStatus::Active:               return L"Active";
    case wddc::DisplaySourceStatus::PoweredOff:           return L"PoweredOff";
    case wddc::DisplaySourceStatus::Invalid:              return L"Invalid";
    case wddc::DisplaySourceStatus::OwnedByAnotherDevice: return L"OwnedByAnotherDevice";
    case wddc::DisplaySourceStatus::Unowned:              return L"Unowned";
    default:                                              return L"<unknown>";
    }
}

const wchar_t* ManagerResultName(wddc::DisplayManagerResult r) {
    switch (r) {
    case wddc::DisplayManagerResult::Success:                   return L"Success";
    case wddc::DisplayManagerResult::UnknownFailure:            return L"UnknownFailure";
    case wddc::DisplayManagerResult::TargetAccessDenied:        return L"TargetAccessDenied";
    case wddc::DisplayManagerResult::TargetStale:               return L"TargetStale";
    case wddc::DisplayManagerResult::RemoteSessionNotSupported: return L"RemoteSessionNotSupported";
    default:                                                    return L"<unknown>";
    }
}

} // namespace

std::wstring MonitorInterfacePathForDevice(const std::wstring& gdiDeviceName) {
    DISPLAY_DEVICEW adapter = {};
    adapter.cb = sizeof(adapter);
    for (DWORD ai = 0; ::EnumDisplayDevicesW(nullptr, ai, &adapter, 0); ++ai) {
        if (::_wcsicmp(adapter.DeviceName, gdiDeviceName.c_str()) != 0) continue;
        DISPLAY_DEVICEW mon = {};
        mon.cb = sizeof(mon);
        if (::EnumDisplayDevicesW(adapter.DeviceName, 0, &mon,
                                  EDD_GET_DEVICE_INTERFACE_NAME)) {
            return mon.DeviceID;
        }
    }
    return std::wstring();
}

// ---------------------------------------------------------------------------

struct SpecializedPresenter::Impl {
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    std::mutex*          ctxMutex = nullptr;

    wddc::DisplayManager     manager{ nullptr };
    wddc::DisplayTarget      target{ nullptr };
    wddc::DisplayState       state{ nullptr };
    wddc::DisplayPath        path{ nullptr };
    wddc::DisplayDevice      displayDevice{ nullptr };
    wddc::DisplayTaskPool    taskPool{ nullptr };
    wddc::DisplaySource      source{ nullptr };

    std::vector<wddc::DisplaySurface>                  surfaces;
    std::vector<wddc::DisplayScanout>                  scanouts;
    std::vector<ComPtr<ID3D11Texture2D>>               textures;
    std::vector<ComPtr<ID3D11RenderTargetView>>        rtvs;

    UINT     width = 0;
    UINT     height = 0;
    uint32_t frame = 0;
    bool     acquired = false;
    bool     ready = false;

    Microsoft::WRL::ComPtr<ID3D11Fence> d3dFence;
    wddc::DisplayFence                  displayFence{ nullptr };
    uint64_t                            fenceValue = 0;

    std::wstring interfacePath;      // cached while still on the desktop
    ULONGLONG    acquiredTick = 0;
    int          frameDeadlineSeconds = 0;

    HANDLE   liveEvent = nullptr;
    bool     liveSignalled = false;
    unsigned consecutiveFailures = 0;
    bool     failed = false;

    void Release() {
        if (liveEvent) {
            ::ResetEvent(liveEvent);
            ::CloseHandle(liveEvent);
            liveEvent = nullptr;
        }
        liveSignalled = false;
        displayFence = nullptr;
        d3dFence.Reset();
        fenceValue = 0;
        rtvs.clear();
        textures.clear();
        scanouts.clear();
        surfaces.clear();
        source = nullptr;
        taskPool = nullptr;
        displayDevice = nullptr;
        path = nullptr;
        state = nullptr;
        if (manager && target && acquired) {
            try {
                manager.ReleaseTarget(target);
                DS_LOG(L"specialized: released display target");
            } catch (const winrt::hresult_error&) {
            }
        }
        acquired = false;
        ready = false;
        target = nullptr;
        manager = nullptr;
    }
};

SpecializedPresenter::SpecializedPresenter() : m_impl(std::make_unique<Impl>()) {}

SpecializedPresenter::~SpecializedPresenter() {
    Shutdown();
}

UINT SpecializedPresenter::Width() const { return m_impl->width; }
UINT SpecializedPresenter::Height() const { return m_impl->height; }

void SpecializedPresenter::RequestResize(UINT, UINT) {
    // The mode is fixed for the lifetime of the acquisition; a display change
    // tears the whole compositor down instead.
}

bool SpecializedPresenter::CacheTargetIdentity(const std::wstring& gdiDeviceName) {
    Impl& s = *m_impl;
    if (!s.interfacePath.empty()) return true;
    s.interfacePath = MonitorInterfacePathForDevice(gdiDeviceName);
    if (s.interfacePath.empty()) {
        DS_WARN(L"specialized: '%s' has no device interface path. dscomp must be "
                L"started BEFORE the display is removed from the desktop.",
                gdiDeviceName.c_str());
        return false;
    }
    DS_VERB(L"specialized: cached target identity %s", s.interfacePath.c_str());
    return true;
}

// One acquisition attempt. Safe to call repeatedly while polling.
bool SpecializedPresenter::TryAcquireOnce(const PresenterInit& init,
                                          SpecializedAvailability& out) {
    Impl& s = *m_impl;
    s.device = init.device;
    s.ctx = init.ctx;
    s.ctxMutex = init.ctxMutex;

    out.requested = true;

    try {
        // The official sample uses None. EnforceSourceOwnership changes source
        // recoverability semantics and is NOT what the supported pattern does.
        s.manager = wddc::DisplayManager::Create(wddc::DisplayManagerOptions::None);
        out.apiAvailable = true;
    } catch (const winrt::hresult_error& ex) {
        out.detail = ex.message();
        return false;
    }

    if (s.interfacePath.empty()) {
        out.detail = L"the physical monitor has no device interface path";
        return false;
    }
    const std::wstring wantKey = DevnodeKey(s.interfacePath);

    wdd::DisplayMonitor matched{ nullptr };
    try {
        for (const auto& t : s.manager.GetCurrentTargets()) {
            if (!t.IsConnected()) continue;
            wdd::DisplayMonitor m{ nullptr };
            try {
                m = t.TryGetMonitor();
            } catch (const winrt::hresult_error&) {
                continue;
            }
            if (!m) continue;
            if (DevnodeKey(std::wstring{ m.DeviceId() }) == wantKey) {
                s.target = t;
                matched = m;
                break;
            }
        }
    } catch (const winrt::hresult_error& ex) {
        out.detail = ex.message();
        return false;
    }
    if (!s.target || !matched) return false;
    out.targetFound = true;

    // UsageKind is ADVISORY ONLY - do not gate on it.
    //
    // A display specialized through the supported route (Settings "Remove
    // display from desktop" / DisplayConfig SET_MONITOR_SPECIALIZATION) is
    // reported by DisplayMonitor as UsageKind::Standard anyway; this is a
    // long-standing, still-open Windows bug
    // (microsoft/Windows-classic-samples issue #191). Refusing to continue on
    // Standard would reject a display that is genuinely acquirable.
    //
    // The authoritative gate is whether acquisition actually succeeds, which
    // the next step performs. Log what we saw and carry on.
    const auto usage = matched.UsageKind();
    DS_LOG(L"specialized: monitor UsageKind is %s (advisory only - acquisition is "
           L"the real test)",
           usage == wdd::DisplayMonitorUsageKind::SpecialPurpose ? L"SpecialPurpose"
           : usage == wdd::DisplayMonitorUsageKind::HeadMounted  ? L"HeadMounted"
                                                                 : L"Standard");
    out.targetSpecialized = true;

    // Acquire and build a fresh display state containing only this target.
    try {
        const auto acquire = s.manager.TryAcquireTargetsAndCreateEmptyState(
            winrt::single_threaded_vector<wddc::DisplayTarget>({ s.target }).GetView());
        if (acquire.ErrorCode() != wddc::DisplayManagerResult::Success) {
            out.detail = ManagerResultName(acquire.ErrorCode());
            return false;
        }
        s.state = acquire.State();
        s.acquired = true;
        out.acquired = true;
    } catch (const winrt::hresult_error& ex) {
        out.detail = ex.message();
        return false;
    }

    // Connect a path and choose the highest refresh rate the display offers.
    try {
        s.path = s.state.ConnectTarget(s.target);
        // These are FILTERS on FindModes and must be set before enumerating.
        s.path.IsInterlaced(false);
        s.path.Scaling(wddc::DisplayPathScaling::Identity);
        s.path.SourcePixelFormat(wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized);

        // With an override we must enumerate everything, not just the
        // preferred resolution.
        const bool overriding = init.lowestMode || init.modeWidth != 0;
        const auto query = overriding
                               ? wddc::DisplayModeQueryOptions::None
                               : wddc::DisplayModeQueryOptions::OnlyPreferredResolution;

        wddc::DisplayModeInfo best{ nullptr };
        double bestRate = 0.0;
        uint64_t bestScore = 0;
        unsigned modeCount = 0;
        for (const auto& mode : s.path.FindModes(query)) {
            const auto res = mode.TargetResolution();
            const auto rate = mode.PresentationRate().VerticalSyncRate;
            const double hz = (rate.Denominator != 0)
                                  ? static_cast<double>(rate.Numerator) /
                                        static_cast<double>(rate.Denominator)
                                  : 0.0;
            const uint64_t pixels = static_cast<uint64_t>(res.Width) *
                                    static_cast<uint64_t>(res.Height);
            ++modeCount;

            if (init.modeWidth != 0) {
                // Exact resolution; nearest refresh within it.
                if (static_cast<uint32_t>(res.Width) != init.modeWidth ||
                    static_cast<uint32_t>(res.Height) != init.modeHeight) {
                    continue;
                }
                if (!best || (init.modeHz > 0.0 &&
                              fabs(hz - init.modeHz) < fabs(bestRate - init.modeHz)) ||
                    (init.modeHz <= 0.0 && hz > bestRate)) {
                    bestRate = hz;
                    best = mode;
                }
                continue;
            }

            // Score: pixels dominate, refresh breaks ties. Lowest or highest.
            const uint64_t score = pixels * 1000u + static_cast<uint64_t>(hz);
            const bool better = !best || (init.lowestMode ? (score < bestScore)
                                                         : (score > bestScore));
            if (better) {
                bestScore = score;
                bestRate = hz;
                best = mode;
            }
        }
        DS_LOG(L"specialized: %u mode(s) advertised; selection policy = %s",
               modeCount,
               init.modeWidth ? L"explicit --mode"
               : init.lowestMode ? L"LOWEST (diagnostic)"
                                 : L"highest");
        if (!best) {
            out.detail = L"the display offered no usable modes";
            return false;
        }
        s.path.ApplyPropertiesFromMode(best);
        const auto res = best.TargetResolution();
        s.width = static_cast<UINT>(res.Width);
        s.height = static_cast<UINT>(res.Height);
        DS_LOG(L"specialized: selected mode %ux%u @ %.3f Hz", s.width, s.height,
               bestRate);

        const auto applied = s.state.TryApply(wddc::DisplayStateApplyOptions::None);
        const auto status = applied.Status();
        const wchar_t* statusName =
            status == wddc::DisplayStateOperationStatus::Success ? L"Success"
            : status == wddc::DisplayStateOperationStatus::PartialFailure ? L"PartialFailure"
            : status == wddc::DisplayStateOperationStatus::UnknownFailure ? L"UnknownFailure"
            : status == wddc::DisplayStateOperationStatus::TargetOwnershipLost ? L"TargetOwnershipLost"
            : status == wddc::DisplayStateOperationStatus::SystemStateChanged ? L"SystemStateChanged"
            : status == wddc::DisplayStateOperationStatus::TooManyPathsForAdapter ? L"TooManyPathsForAdapter"
            : status == wddc::DisplayStateOperationStatus::ModesNotSupported ? L"ModesNotSupported"
            : status == wddc::DisplayStateOperationStatus::RemoteSessionNotSupported ? L"RemoteSessionNotSupported"
            : L"<unknown>";
        DS_LOG(L"specialized: TryApply status = %s (%d), extendedErrorCode 0x%08X",
               statusName, static_cast<int>(status),
               static_cast<unsigned>(applied.ExtendedErrorCode()));
        if (status != wddc::DisplayStateOperationStatus::Success) {
            out.detail = std::wstring(L"TryApply: ") + statusName;
            return false;
        }

        // Re-read the applied state. Anything built from the pre-apply path can
        // carry stale or unset resolution/format.
        const auto reread = s.manager.TryAcquireTargetsAndReadCurrentState(
            winrt::single_threaded_vector<wddc::DisplayTarget>({ s.target }).GetView());
        if (reread.ErrorCode() != wddc::DisplayManagerResult::Success) {
            out.detail = std::wstring(L"re-read state: ") +
                         ManagerResultName(reread.ErrorCode());
            return false;
        }
        s.state = reread.State();
        s.path = s.state.GetPathForTarget(s.target);
        DS_LOG(L"specialized: re-read applied state OK");
    } catch (const winrt::hresult_error& ex) {
        out.detail = ex.message();
        return false;
    }

    // Build the presentation objects and share the primaries with our D3D11
    // device so the renderer can draw straight into them.
    try {
        // The render device MUST live on the adapter that drives the acquired
        // target, or the shared primaries belong to a different GPU.
        const auto adapterId = s.target.Adapter().Id();
        DXGI_ADAPTER_DESC renderDesc = {};
        {
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> renderAdapter;
            if (SUCCEEDED(s.device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
                SUCCEEDED(dxgiDevice->GetAdapter(&renderAdapter))) {
                renderAdapter->GetDesc(&renderDesc);
            }
        }
        if (renderDesc.AdapterLuid.LowPart != adapterId.LowPart ||
            renderDesc.AdapterLuid.HighPart != adapterId.HighPart) {
            DS_ERR(L"specialized: ADAPTER MISMATCH - the render device is on LUID "
                   L"%08X:%08X but the acquired target is driven by LUID %08X:%08X. "
                   L"Shared primaries cannot work across this boundary.",
                   renderDesc.AdapterLuid.HighPart, renderDesc.AdapterLuid.LowPart,
                   adapterId.HighPart, adapterId.LowPart);
            out.detail = L"render device is on a different adapter than the target";
            return false;
        }
        DS_VERB(L"specialized: render device and target share adapter LUID %08X:%08X",
                adapterId.HighPart, adapterId.LowPart);

        s.displayDevice = s.manager.CreateDisplayDevice(s.target.Adapter());
        s.taskPool = s.displayDevice.CreateTaskPool();
        s.source = s.displayDevice.CreateScanoutSource(s.target);

        // Every field read back from the APPLIED path, as the sample does.
        const auto srcRes = s.path.SourceResolution().Value();
        s.width = static_cast<UINT>(srcRes.Width);
        s.height = static_cast<UINT>(srcRes.Height);
        const wddc::DisplayPrimaryDescription desc{
            static_cast<uint32_t>(srcRes.Width), static_cast<uint32_t>(srcRes.Height),
            s.path.SourcePixelFormat(),
            wgdx::DirectXColorSpace::RgbFullG22NoneP709, false,
            winrt::Windows::Graphics::DirectX::Direct3D11::Direct3DMultisampleDescription{ 1, 0 }
        };
        DS_LOG(L"specialized: primaries %ux%u from the applied path", s.width,
               s.height);

        auto interop = s.displayDevice.as<::IDisplayDeviceInterop>();

        // Scanout must not begin before the GPU has finished writing the
        // primary. Share a D3D11 fence with the display engine and make every
        // task wait on it. Without this the display stack can fault the moment
        // the first task is executed.
        ComPtr<ID3D11Device5> dev5;
        if (SUCCEEDED(s.device->QueryInterface(IID_PPV_ARGS(&dev5)))) {
            if (SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                            IID_PPV_ARGS(&s.d3dFence)))) {
                HANDLE fenceHandle = nullptr;
                if (SUCCEEDED(s.d3dFence->CreateSharedHandle(nullptr, GENERIC_ALL,
                                                             nullptr, &fenceHandle))) {
                    winrt::com_ptr<::IInspectable> insp;
                    const HRESULT ohr = interop->OpenSharedHandle(
                        fenceHandle, winrt::guid_of<wddc::DisplayFence>(),
                        insp.put_void());
                    ::CloseHandle(fenceHandle);
                    if (SUCCEEDED(ohr) && insp) {
                        s.displayFence = insp.as<wddc::DisplayFence>();
                        DS_VERB(L"specialized: shared scanout fence created");
                    }
                }
            }
        }
        if (!s.displayFence) {
            DS_WARN(L"specialized: could not create a shared scanout fence - "
                    L"presenting without one is not the supported model and may "
                    L"fault the display stack");
        }
        ComPtr<ID3D11Device1> device1;
        if (FAILED(s.device->QueryInterface(IID_PPV_ARGS(&device1)))) {
            out.detail = L"ID3D11Device1 is unavailable";
            return false;
        }

        for (uint32_t i = 0; i < kPrimaryCount; ++i) {
            wddc::DisplaySurface surface =
                s.displayDevice.CreatePrimary(s.target, desc);
            HANDLE shared = nullptr;
            const HRESULT hr = interop->CreateSharedHandle(
                surface.as<::IInspectable>().get(), nullptr, GENERIC_ALL, nullptr,
                &shared);
            if (FAILED(hr) || shared == nullptr) {
                out.detail = L"CreateSharedHandle for a primary failed";
                return false;
            }
            ComPtr<ID3D11Texture2D> tex;
            const HRESULT ohr = device1->OpenSharedResource1(shared, IID_PPV_ARGS(&tex));
            ::CloseHandle(shared);
            if (FAILED(ohr)) {
                out.detail = L"OpenSharedResource1 for a primary failed";
                return false;
            }
            ComPtr<ID3D11RenderTargetView> rtv;
            if (FAILED(s.device->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) {
                out.detail = L"CreateRenderTargetView for a primary failed";
                return false;
            }
            // Scanouts are created ONCE per primary and reused, not rebuilt
            // every frame. syncInterval = 1.
            s.scanouts.push_back(s.displayDevice.CreateSimpleScanout(
                s.source, surface, 0 /*subResourceIndex*/, 1 /*syncInterval*/));
            s.surfaces.push_back(std::move(surface));
            s.textures.push_back(std::move(tex));
            s.rtvs.push_back(std::move(rtv));
        }
        DS_LOG(L"specialized: %zu scanout(s) created up front", s.scanouts.size());
    } catch (const winrt::hresult_error& ex) {
        out.detail = ex.message();
        return false;
    }

    s.liveEvent = CreateLivenessEvent();
    s.acquiredTick = ::GetTickCount64();

    s.ready = true;
    out.presentationReady = true;
    DS_LOG(L"specialized: acquired '%s' at %ux%u with %u primaries",
           init.cfg->physicalDevice.c_str(), s.width, s.height, kPrimaryCount);
    return true;
}

// Polls until the display becomes acquirable, or the wait expires.
bool SpecializedPresenter::TryAcquire(const PresenterInit& init,
                                      SpecializedAvailability& out, int waitSeconds,
                                      int frameDeadlineSeconds) {
    Impl& s = *m_impl;
    s.frameDeadlineSeconds = frameDeadlineSeconds;

    CacheTargetIdentity(init.cfg->physicalDevice);

    const ULONGLONG start = ::GetTickCount64();
    ULONGLONG lastLog = 0;
    bool announced = false;

    for (;;) {
        SpecializedAvailability attempt;
        attempt.requested = true;
        if (TryAcquireOnce(init, attempt)) {
            out = attempt;
            return true;
        }
        // Keep the furthest-progressed diagnosis for the fallback message.
        out = attempt;

        const double elapsed =
            static_cast<double>(::GetTickCount64() - start) / 1000.0;
        if (!ShouldKeepWaiting(waitSeconds, elapsed, false)) {
            if (waitSeconds > 0) {
                DS_WARN(L"specialized: gave up waiting after %d second(s) - '%s' "
                        L"never became acquirable",
                        waitSeconds, init.cfg->physicalDevice.c_str());
            }
            return false;
        }

        // Release whatever the failed attempt part-built before sleeping.
        s.Release();

        const ULONGLONG now = ::GetTickCount64();
        if (!announced) {
            announced = true;
            DS_LOG(L"specialized: WAITING up to %d second(s) for '%s' to be removed "
                   L"from the desktop.", waitSeconds,
                   init.cfg->physicalDevice.c_str());
            DS_LOG(L"specialized: now go to Settings > System > Display > Advanced "
                   L"display and turn ON 'Remove display from desktop'.");
        }
        if (now - lastLog >= kWaitLogEveryMs) {
            lastLog = now;
            const SpecializationBits bits = QuerySpecialization(s.interfacePath);
            const wchar_t* state = L"state unknown";
            if (bits.valid) {
                state = bits.enabled ? L"specialized, acquiring"
                      : (bits.availableForMonitor && bits.availableForSystem)
                            ? L"still on the desktop (eligible)"
                            : L"still on the desktop (NOT eligible!)";
            }
            DS_LOG(L"specialized: waiting for '%s'... %ds left  [%s]",
                   init.cfg->physicalDevice.c_str(),
                   WaitSecondsRemaining(waitSeconds, elapsed), state);
        }
        ::Sleep(kWaitPollMs);
    }
}

bool SpecializedPresenter::Initialize(const PresenterInit& init) {
    // TryAcquire does all the work; Initialize just confirms it ran.
    (void)init;
    return m_impl->ready;
}

ID3D11RenderTargetView* SpecializedPresenter::BeginFrame() {
    Impl& s = *m_impl;
    if (!s.ready || s.rtvs.empty()) return nullptr;
    return s.rtvs[s.frame % s.rtvs.size()].Get();
}

HRESULT SpecializedPresenter::EndFrame() {
    Impl& s = *m_impl;
    if (!s.ready || s.surfaces.empty()) return E_FAIL;

    const size_t index = s.frame % s.surfaces.size();
    const wchar_t* stage = L"<none>";
    try {
        stage = L"ctx->Flush";
        {
            // Make sure the draws for this primary have been submitted before
            // the display engine is told to scan it out.
            std::lock_guard<std::mutex> lock(*s.ctxMutex);
            s.ctx->Flush();
        }
        // Tell the display engine when this primary is safe to scan out.
        if (s.d3dFence && s.displayFence) {
            ComPtr<ID3D11DeviceContext4> ctx4;
            if (SUCCEEDED(s.ctx->QueryInterface(IID_PPV_ARGS(&ctx4)))) {
                std::lock_guard<std::mutex> lock(*s.ctxMutex);
                ctx4->Signal(s.d3dFence.Get(), ++s.fenceValue);
            }
        }
        stage = L"CreateTask";
        auto task = s.taskPool.CreateTask();
        stage = L"SetScanout";
        task.SetScanout(s.scanouts[index]);
        if (s.displayFence && s.fenceValue != 0) {
            stage = L"SetWait";
            task.SetWait(s.displayFence, s.fenceValue);
        }

        // TryExecuteTask returns a structured result instead of throwing an
        // opaque HRESULT - this is what tells us WHY a present was refused.
        stage = L"TryExecuteTask";
        const auto result = s.taskPool.TryExecuteTask(task);
        const auto present = result.PresentStatus();
        if (present != wddc::DisplayPresentStatus::Success) {
            ++s.consecutiveFailures;
            if (s.consecutiveFailures == 1) {
                DS_ERR(L"specialized: TryExecuteTask REFUSED the present - "
                       L"PresentStatus = %s, SourceStatus = %s (frame %u, "
                       L"primary %zu of %zu)",
                       PresentStatusName(present),
                       SourceStatusName(result.SourceStatus()), s.frame, index,
                       s.surfaces.size());
                if (present == wddc::DisplayPresentStatus::DeviceInvalid ||
                    present == wddc::DisplayPresentStatus::UnknownFailure) {
                    DS_ERR(L"specialized:   -> points at DRIVER NON-SUPPORT for "
                           L"direct scanout on this adapter.");
                } else if (present ==
                           wddc::DisplayPresentStatus::SourceStatusPreventedPresent) {
                    DS_ERR(L"specialized:   -> the SOURCE blocked it; see "
                           L"SourceStatus above (ownership problem on our side).");
                }
            }
            if (!s.failed &&
                s.consecutiveFailures >= kMaxConsecutiveScanoutFailures) {
                s.failed = true;
                DS_ERR(L"specialized: giving up after %u refused presents",
                       s.consecutiveFailures);
            }
            return E_FAIL;
        }

        // The sample's only pacing: one vblank wait per loop.
        stage = L"WaitForVBlank";
        s.displayDevice.WaitForVBlank(s.source);
        stage = L"<completed>";
    } catch (const winrt::hresult_error& ex) {
        ++s.consecutiveFailures;
        if (s.consecutiveFailures == 1) {
            DS_ERR(L"specialized: FAILED IN '%s' (frame %u, primary %zu of %zu): "
                   L"0x%08X %s",
                   stage, s.frame, index, s.surfaces.size(),
                   static_cast<unsigned>(ex.code()), ex.message().c_str());
        }
        if (!s.failed && s.consecutiveFailures >= kMaxConsecutiveScanoutFailures) {
            s.failed = true;
            DS_ERR(L"specialized: the display stopped accepting scanouts (%u "
                   L"consecutive failures, last 0x%08X). Treating the hidden "
                   L"display as LOST - shutting down so the control app can "
                   L"un-specialize it.",
                   s.consecutiveFailures, static_cast<unsigned>(ex.code()));
        }
        return ex.code();
    }

    s.consecutiveFailures = 0;
    ++s.frame;

    // First frame that actually reached the display: this is the signal the
    // control app's keep/revert guard waits for.
    if (!s.liveSignalled) {
        s.liveSignalled = true;
        if (s.liveEvent) ::SetEvent(s.liveEvent);
        DS_LOG(L"specialized: FIRST FRAME SCANNED OUT - signalled %s%s",
               L"DeskSplitSpecializedLive",
               s.liveEvent ? L"" : L" (event unavailable; nothing signalled)");
    }
    return S_OK;
}

bool SpecializedPresenter::Failed() const {
    if (!m_impl) return false;
    Impl& s = *m_impl;
    if (s.failed) return true;

    // Acquired, but nothing has ever reached the panel. That is a black screen
    // with no end in sight, so bound it.
    if (s.ready && !s.liveSignalled && s.acquiredTick != 0) {
        const double since =
            static_cast<double>(::GetTickCount64() - s.acquiredTick) / 1000.0;
        if (FrameDeadlineExceeded(s.frameDeadlineSeconds, s.liveSignalled, since)) {
            s.failed = true;
            DS_ERR(L"specialized: acquired the display but no frame was scanned out "
                   L"within %d second(s). The panel is specialized and BLACK - "
                   L"releasing it and exiting so the control app can un-specialize.",
                   s.frameDeadlineSeconds);
        }
    }
    return s.failed;
}

void SpecializedPresenter::Shutdown() {
    if (m_impl) m_impl->Release();
}

} // namespace ds
