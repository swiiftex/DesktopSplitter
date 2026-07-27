// WindowPresenter.h - the original flip-model swapchain presentation path.
#pragma once

#include "Presenter.h"

#include <wrl/client.h>
#include <atomic>

namespace ds {

class WindowPresenter final : public IPresenter {
public:
    const wchar_t* Name() const override { return L"window"; }

    bool Initialize(const PresenterInit& init) override;
    void Shutdown() override;
    void RequestResize(UINT width, UINT height) override;
    ID3D11RenderTargetView* BeginFrame() override;
    HRESULT EndFrame() override;

    UINT Width() const override { return m_width; }
    UINT Height() const override { return m_height; }

    // Present(1,0) waits for v-blank; the tearing path does not.
    bool SelfPaced() const override { return !m_tearingSupported; }

private:
    bool CreateBackBufferView();
    bool HandlePendingResize();

    ID3D11Device*        m_device = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    std::mutex*          m_ctxMutex = nullptr;

    Microsoft::WRL::ComPtr<IDXGISwapChain1>        m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;

    UINT m_width = 0;
    UINT m_height = 0;
    bool m_tearingSupported = false;
    UINT m_swapChainFlags = 0;

    std::atomic<uint32_t> m_pendingResizeW{ 0 };
    std::atomic<uint32_t> m_pendingResizeH{ 0 };
};

} // namespace ds
