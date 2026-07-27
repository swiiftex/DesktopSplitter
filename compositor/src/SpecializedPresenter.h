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
    bool TryAcquire(const PresenterInit& init, SpecializedAvailability& out);

    bool Initialize(const PresenterInit& init) override;
    void Shutdown() override;
    void RequestResize(UINT width, UINT height) override;
    ID3D11RenderTargetView* BeginFrame() override;
    HRESULT EndFrame() override;

    UINT Width() const override;
    UINT Height() const override;

    // Scanout is submitted with a sync interval, so the display paces us.
    bool SelfPaced() const override { return true; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Resolves a GDI device name ("\\.\DISPLAY1") to the monitor device interface
// path Display.Core reports as DisplayMonitor.DeviceId. Empty on failure.
std::wstring MonitorInterfacePathForDevice(const std::wstring& gdiDeviceName);

} // namespace ds
