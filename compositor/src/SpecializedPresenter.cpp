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
#include <algorithm>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace wdd = winrt::Windows::Devices::Display;
namespace wddc = winrt::Windows::Devices::Display::Core;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace ds {

namespace {

constexpr uint32_t kPrimaryCount = 2;   // double-buffered scanout

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
    std::vector<ComPtr<ID3D11Texture2D>>               textures;
    std::vector<ComPtr<ID3D11RenderTargetView>>        rtvs;

    UINT     width = 0;
    UINT     height = 0;
    uint32_t frame = 0;
    bool     acquired = false;
    bool     ready = false;

    void Release() {
        rtvs.clear();
        textures.clear();
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

bool SpecializedPresenter::TryAcquire(const PresenterInit& init,
                                      SpecializedAvailability& out) {
    Impl& s = *m_impl;
    s.device = init.device;
    s.ctx = init.ctx;
    s.ctxMutex = init.ctxMutex;

    out.requested = true;

    try {
        s.manager = wddc::DisplayManager::Create(wddc::DisplayManagerOptions::None);
        out.apiAvailable = true;
    } catch (const winrt::hresult_error& ex) {
        out.detail = ex.message();
        return false;
    }

    const std::wstring wantPath =
        MonitorInterfacePathForDevice(init.cfg->physicalDevice);
    if (wantPath.empty()) {
        out.detail = L"the physical monitor has no device interface path";
        return false;
    }
    const std::wstring wantKey = DevnodeKey(wantPath);

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

    if (matched.UsageKind() != wdd::DisplayMonitorUsageKind::SpecialPurpose) {
        out.detail = L"UsageKind is not SpecialPurpose";
        return false;
    }
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
        s.path.IsInterlaced(false);
        s.path.Scaling(wddc::DisplayPathScaling::Identity);

        wddc::DisplayModeInfo best{ nullptr };
        double bestRate = 0.0;
        uint32_t bestPixels = 0;
        for (const auto& mode : s.path.FindModes(wddc::DisplayModeQueryOptions::OnlyPreferredResolution)) {
            const auto res = mode.TargetResolution();
            const auto rate = mode.PresentationRate().VerticalSyncRate;
            const double hz = (rate.Denominator != 0)
                                  ? static_cast<double>(rate.Numerator) /
                                        static_cast<double>(rate.Denominator)
                                  : 0.0;
            const uint32_t pixels = static_cast<uint32_t>(res.Width) *
                                    static_cast<uint32_t>(res.Height);
            if (pixels > bestPixels || (pixels == bestPixels && hz > bestRate)) {
                bestPixels = pixels;
                bestRate = hz;
                best = mode;
            }
        }
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
        if (applied.Status() != wddc::DisplayStateOperationStatus::Success) {
            out.detail = L"TryApply did not succeed";
            return false;
        }
    } catch (const winrt::hresult_error& ex) {
        out.detail = ex.message();
        return false;
    }

    // Build the presentation objects and share the primaries with our D3D11
    // device so the renderer can draw straight into them.
    try {
        s.displayDevice = s.manager.CreateDisplayDevice(s.target.Adapter());
        s.taskPool = s.displayDevice.CreateTaskPool();
        s.source = s.displayDevice.CreateScanoutSource(s.target);

        const winrt::Windows::Graphics::SizeInt32 size{
            static_cast<int32_t>(s.width), static_cast<int32_t>(s.height)
        };
        const wddc::DisplayPrimaryDescription desc{
            static_cast<uint32_t>(s.width), static_cast<uint32_t>(s.height),
            wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            wgdx::DirectXColorSpace::RgbFullG22NoneP709, false,
            winrt::Windows::Graphics::DirectX::Direct3D11::Direct3DMultisampleDescription{ 1, 0 }
        };
        (void)size;

        auto interop = s.displayDevice.as<::IDisplayDeviceInterop>();
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
            s.surfaces.push_back(std::move(surface));
            s.textures.push_back(std::move(tex));
            s.rtvs.push_back(std::move(rtv));
        }
    } catch (const winrt::hresult_error& ex) {
        out.detail = ex.message();
        return false;
    }

    s.ready = true;
    out.presentationReady = true;
    DS_LOG(L"specialized: acquired '%s' at %ux%u with %u primaries",
           init.cfg->physicalDevice.c_str(), s.width, s.height, kPrimaryCount);
    return true;
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
    try {
        {
            // Make sure the draws for this primary have been submitted before
            // the display engine is told to scan it out.
            std::lock_guard<std::mutex> lock(*s.ctxMutex);
            s.ctx->Flush();
        }
        const auto scanout = s.displayDevice.CreateSimpleScanout(
            s.source, s.surfaces[index], 0 /*subResourceIndex*/, 1 /*syncInterval*/);
        auto task = s.taskPool.CreateTask();
        task.SetScanout(scanout);
        s.taskPool.ExecuteTask(task);
    } catch (const winrt::hresult_error& ex) {
        DS_WARN(L"specialized: present failed: 0x%08X %s",
                static_cast<unsigned>(ex.code()), ex.message().c_str());
        return ex.code();
    }
    ++s.frame;
    return S_OK;
}

void SpecializedPresenter::Shutdown() {
    if (m_impl) m_impl->Release();
}

} // namespace ds
