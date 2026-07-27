#include "WindowPresenter.h"
#include "Log.h"

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace ds {

bool WindowPresenter::Initialize(const PresenterInit& init) {
    m_device = init.device;
    m_ctx = init.ctx;
    m_ctxMutex = init.ctxMutex;
    if (!m_device || !m_ctx || !init.factory || !init.hwnd) {
        DS_ERR(L"window presenter: missing initialisation parameters");
        return false;
    }

    BOOL allowTearing = FALSE;
    if (SUCCEEDED(init.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &allowTearing,
                                                    sizeof(allowTearing)))) {
        m_tearingSupported = allowTearing != FALSE;
    }
    DS_LOG(L"tearing support: %s", m_tearingSupported ? L"yes" : L"no");

    RECT rc = {};
    ::GetClientRect(init.hwnd, &rc);
    m_width = static_cast<UINT>(std::max<LONG>(1, rc.right - rc.left));
    m_height = static_cast<UINT>(std::max<LONG>(1, rc.bottom - rc.top));

    m_swapChainFlags = m_tearingSupported
                           ? static_cast<UINT>(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)
                           : 0u;

    DXGI_SWAP_CHAIN_DESC1 sc = {};
    sc.Width = m_width;
    sc.Height = m_height;
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.Stereo = FALSE;
    sc.SampleDesc.Count = 1;
    sc.SampleDesc.Quality = 0;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.Scaling = DXGI_SCALING_STRETCH;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sc.Flags = m_swapChainFlags;

    const HRESULT hr = init.factory->CreateSwapChainForHwnd(
        m_device, init.hwnd, &sc, nullptr, nullptr, &m_swapChain);
    if (FAILED(hr)) {
        DS_ERR(L"CreateSwapChainForHwnd failed: %s", HrString(hr));
        return false;
    }
    init.factory->MakeWindowAssociation(
        init.hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    DS_LOG(L"swapchain %ux%u FLIP_DISCARD B8G8R8A8_UNORM", m_width, m_height);

    return CreateBackBufferView();
}

bool WindowPresenter::CreateBackBufferView() {
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        DS_ERR(L"swapchain GetBuffer failed: %s", HrString(hr));
        return false;
    }
    hr = m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
    if (FAILED(hr)) {
        DS_ERR(L"CreateRenderTargetView failed: %s", HrString(hr));
        return false;
    }
    return true;
}

void WindowPresenter::RequestResize(UINT width, UINT height) {
    m_pendingResizeW.store(width, std::memory_order_release);
    m_pendingResizeH.store(height, std::memory_order_release);
}

bool WindowPresenter::HandlePendingResize() {
    const UINT w = m_pendingResizeW.exchange(0, std::memory_order_acq_rel);
    const UINT h = m_pendingResizeH.exchange(0, std::memory_order_acq_rel);
    if (w == 0 || h == 0) return true;
    if (w == m_width && h == m_height) return true;

    std::lock_guard<std::mutex> lock(*m_ctxMutex);
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_rtv.Reset();
    const HRESULT hr =
        m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, m_swapChainFlags);
    if (FAILED(hr)) {
        DS_ERR(L"ResizeBuffers(%ux%u) failed: %s", w, h, HrString(hr));
        return CreateBackBufferView();
    }
    m_width = w;
    m_height = h;
    DS_LOG(L"swapchain resized to %ux%u", w, h);
    return CreateBackBufferView();
}

ID3D11RenderTargetView* WindowPresenter::BeginFrame() {
    if (!HandlePendingResize()) return nullptr;
    return m_rtv.Get();
}

HRESULT WindowPresenter::EndFrame() {
    if (!m_swapChain) return E_FAIL;
    return m_tearingSupported ? m_swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING)
                              : m_swapChain->Present(1, 0);
}

void WindowPresenter::Shutdown() {
    m_rtv.Reset();
    m_swapChain.Reset();
    m_device = nullptr;
    m_ctx = nullptr;
    m_ctxMutex = nullptr;
}

} // namespace ds
