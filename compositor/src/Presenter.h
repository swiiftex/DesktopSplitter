// Presenter.h - how a composed frame reaches the physical display.
//
// Two implementations:
//   WindowPresenter      - flip-model DXGI swapchain on a borderless topmost
//                          window. The original, proven path.
//   SpecializedPresenter - Windows.Devices.Display.Core: the monitor is removed
//                          from the desktop entirely and driven directly.
//                          Requires the monitor to be marked "specialized".
//
// The renderer draws exactly the same composite either way; only BeginFrame /
// EndFrame differ.
#pragma once

#include "Common.h"

#include <d3d11.h>
#include <dxgi1_5.h>

#include <mutex>

namespace ds {

struct PresenterInit {
    HWND                 hwnd = nullptr;      // window path only
    const AppConfig*     cfg = nullptr;
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIFactory5*       factory = nullptr;   // window path only
    IDXGIAdapter1*       adapter = nullptr;
    std::mutex*          ctxMutex = nullptr;  // guards 'ctx'

    // Diagnostic mode override for the specialized path. 5120x1440@240 is a
    // DSC-class mode; being able to ask for the smallest advertised mode tells
    // us whether a failure is bandwidth-related or API-usage-related.
    bool     lowestMode = false;         // --lowest-mode
    uint32_t modeWidth = 0;              // --mode WxH@Hz (0 = no override)
    uint32_t modeHeight = 0;
    double   modeHz = 0.0;
};

class IPresenter {
public:
    virtual ~IPresenter() = default;

    virtual const wchar_t* Name() const = 0;
    virtual bool Initialize(const PresenterInit& init) = 0;
    virtual void Shutdown() = 0;

    // Window path only; ignored elsewhere.
    virtual void RequestResize(UINT width, UINT height) = 0;

    // Prepares this frame's target. Returns nullptr to skip the frame.
    virtual ID3D11RenderTargetView* BeginFrame() = 0;

    // Presents. Returns the underlying HRESULT for diagnostics.
    virtual HRESULT EndFrame() = 0;

    virtual UINT Width() const = 0;
    virtual UINT Height() const = 0;

    // True when presentation blocks until the display is ready, so the render
    // loop must not additionally pace itself.
    virtual bool SelfPaced() const = 0;

    // True once the presenter has failed unrecoverably *after* it was already
    // presenting - e.g. the specialized display stopped accepting scanouts.
    // The render loop stops and the process exits with a distinct code so the
    // control app can tell "the hidden display went dark" from a clean exit.
    virtual bool Failed() const { return false; }
};

} // namespace ds
