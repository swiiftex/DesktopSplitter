// Renderer.h - D3D11 device, flip-model swapchain and the composition thread.
#pragma once

#include "Common.h"
#include "SegmentShared.h"
#include "Presenter.h"
#include "PresenterSelect.h"

#include <dxgi1_5.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <memory>
#include <thread>

namespace ds {

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Creates the device, presenter and pipeline state. Must be called before
    // capture threads start (they share the device). When 'wantSpecialized' is
    // set the specialized display path is attempted first, falling back to the
    // window swapchain with a logged reason.
    bool Init(HWND hwnd, const AppConfig& cfg, std::vector<SegmentShared>* segments,
              bool wantSpecialized);
    void Shutdown();

    bool StartThread();
    void StopThread();

    // Called from the window thread when the physical monitor rect changes.
    void RequestResize(UINT width, UINT height);

    GpuContext* Gpu() { return &m_gpu; }

    // Which presentation path was chosen, and why.
    const PresenterSelection& Selection() const { return m_selection; }

private:
    struct CursorGpu {
        Microsoft::WRL::ComPtr<ID3D11Texture2D>          tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        UINT     width = 0;
        UINT     height = 0;
        uint64_t version = 0;
    };

    bool CreateDevice();
    bool CreatePresenter(HWND hwnd, bool wantSpecialized);
    bool CreatePipeline();
    void RenderFrame();
    void DrawQuad(ID3D11ShaderResourceView* srv, float px, float py, float pw, float ph);
    void ThreadMain();
    void PaceFrame();

    HWND        m_hwnd = nullptr;
    AppConfig   m_cfg;
    std::vector<SegmentShared>* m_segments = nullptr;

    GpuContext m_gpu;
    Microsoft::WRL::ComPtr<IDXGIFactory5>       m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>       m_adapter;

    Microsoft::WRL::ComPtr<ID3D11VertexShader>  m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>   m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer>        m_cb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>  m_sampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState>    m_blendOpaque;
    Microsoft::WRL::ComPtr<ID3D11BlendState>    m_blendAlpha;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_raster;

    std::vector<CursorGpu> m_cursors;

    std::unique_ptr<IPresenter> m_presenter;
    PresenterSelection          m_selection;

    std::atomic<bool> m_stop{false};
    std::thread       m_thread;

    LARGE_INTEGER m_qpcFreq = {};
    long long     m_nextPresentQpc = 0;
    HANDLE        m_paceTimer = nullptr;
};

} // namespace ds
