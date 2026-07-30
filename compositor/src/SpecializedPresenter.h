// SpecializedPresenter.h - drives the physical monitor directly through
// Windows.Devices.Display.Core, with the display removed from the desktop.
//
// EXPERIMENTAL. Requires the monitor to be marked as a specialized display
// (DisplayMonitorUsageKind::SpecialPurpose), which in turn requires the
// Microsoft VSDB in its EDID - see docs/HIDING.md and installer/edid-override.
// Every step reports failure through SpecializedAvailability so the caller can
// fall back to WindowPresenter.
#pragma once

#include "Presenter.h"
#include "PresenterSelect.h"

#include <wrl/client.h>
#include <memory>

namespace ds {

class SpecializedPresenter final : public IPresenter {
public:
    SpecializedPresenter();
    ~SpecializedPresenter() override;

    const wchar_t* Name() const override { return L"specialized"; }

    // Fills 'out' with exactly how far acquisition got, so the caller can log a
    // precise fallback reason. Returns true only when fully ready to present.
    //
    // 'waitSeconds' > 0 polls until the display becomes acquirable, which is
    // what lets the user start dscomp FIRST and then remove the display from
    // the desktop: the black gap is then a blink rather than an open-ended dark
    // screen. 0 means a single attempt (today's behaviour).
    //
    // 'frameDeadlineSeconds' bounds the "acquired but never presents" case: a
    // specialized display receiving no frames is just a black panel, so after
    // the deadline Failed() goes true and the process exits kDisplayLost.
    bool TryAcquire(const PresenterInit& init, SpecializedAvailability& out,
                    int waitSeconds, int frameDeadlineSeconds);

    // Resolves and caches the monitor's device interface path. MUST be called
    // while the display is still on the desktop - once it is specialized it
    // disappears from EnumDisplayDevices and can no longer be resolved by GDI
    // device name.
    bool CacheTargetIdentity(const std::wstring& gdiDeviceName);

    bool Initialize(const PresenterInit& init) override;
    void Shutdown() override;
    void RequestResize(UINT width, UINT height) override;
    ID3D11RenderTargetView* BeginFrame() override;
    HRESULT EndFrame() override;

    UINT Width() const override;
    UINT Height() const override;

    // Scanout is submitted with a sync interval, so the display paces us.
    bool SelfPaced() const override { return true; }

    // True once the display has stopped accepting scanouts for long enough to
    // call it lost. The compositor then exits with exitcode::kDisplayLost.
    bool Failed() const override;

private:
    bool TryAcquireOnce(const PresenterInit& init, SpecializedAvailability& out);

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Resolves a GDI device name ("\\.\DISPLAY1") to the monitor device interface
// path Display.Core reports as DisplayMonitor.DeviceId. Empty on failure.
std::wstring MonitorInterfacePathForDevice(const std::wstring& gdiDeviceName);

} // namespace ds
