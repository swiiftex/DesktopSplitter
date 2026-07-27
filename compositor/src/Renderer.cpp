#include "Renderer.h"
#include "WindowPresenter.h"
#include "SpecializedPresenter.h"
#include "Log.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace ds {

namespace {

// One quad, no vertex buffer: positions come from SV_VertexID and the cbuffer.
const char kShaderSource[] =
    "cbuffer QuadCB : register(b0)\n"
    "{\n"
    "    float4 gPos;  // x,y = top-left in NDC, z,w = size in NDC (positive)\n"
    "    float4 gUv;   // x,y = uv origin,       z,w = uv size\n"
    "};\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut VSMain(uint vid : SV_VertexID)\n"
    "{\n"
    "    float2 c = float2((float)(vid & 1u), (float)((vid >> 1u) & 1u));\n"
    "    VSOut o;\n"
    "    o.pos = float4(gPos.x + c.x * gPos.z, gPos.y - c.y * gPos.w, 0.0f, 1.0f);\n"
    "    o.uv  = float2(gUv.x + c.x * gUv.z, gUv.y + c.y * gUv.w);\n"
    "    return o;\n"
    "}\n"
    "Texture2D    gTex : register(t0);\n"
    "SamplerState gSmp : register(s0);\n"
    "float4 PSMain(VSOut i) : SV_Target\n"
    "{\n"
    "    return gTex.Sample(gSmp, i.uv);\n"
    "}\n";

struct QuadCB {
    float pos[4];
    float uv[4];
};

bool CompileShader(const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
    ComPtr<ID3DBlob> errors;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG;
#endif
    const HRESULT hr = ::D3DCompile(kShaderSource, sizeof(kShaderSource) - 1,
                                    "dscomp.hlsl", nullptr, nullptr, entry, target,
                                    flags, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            DS_ERR(L"shader '%S' compile failed: %S", entry,
                   static_cast<const char*>(errors->GetBufferPointer()));
        } else {
            DS_ERR(L"shader '%S' compile failed: %s", entry, HrString(hr));
        }
        return false;
    }
    return true;
}

// Finds the adapter that drives 'deviceName'; falls back to adapter 0.
bool PickAdapter(IDXGIFactory5* factory, const std::wstring& deviceName,
                 ComPtr<IDXGIAdapter1>& outAdapter) {
    for (UINT ai = 0;; ++ai) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(ai, &a) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT oi = 0;; ++oi) {
            ComPtr<IDXGIOutput> o;
            if (a->EnumOutputs(oi, &o) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od = {};
            if (FAILED(o->GetDesc(&od))) continue;
            if (::_wcsicmp(od.DeviceName, deviceName.c_str()) == 0) {
                outAdapter = a;
                return true;
            }
        }
    }
    DS_WARN(L"physical device '%s' not matched to an adapter; using adapter 0",
            deviceName.c_str());
    ComPtr<IDXGIAdapter1> a0;
    if (factory->EnumAdapters1(0, &a0) == DXGI_ERROR_NOT_FOUND) return false;
    outAdapter = a0;
    return true;
}

} // namespace

Renderer::~Renderer() {
    StopThread();
    Shutdown();
}

bool Renderer::Init(HWND hwnd, const AppConfig& cfg,
                    std::vector<SegmentShared>* segments, bool wantSpecialized) {
    m_hwnd = hwnd;
    m_cfg = cfg;
    m_segments = segments;
    m_cursors.resize(cfg.segments.size());
    ::QueryPerformanceFrequency(&m_qpcFreq);

    m_paceTimer = ::CreateWaitableTimerExW(nullptr, nullptr,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
    if (!m_paceTimer) {
        m_paceTimer = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    if (!CreateDevice()) return false;
    if (!CreatePresenter(hwnd, wantSpecialized)) return false;
    if (!CreatePipeline()) return false;
    return true;
}

bool Renderer::CreateDevice() {
    UINT factoryFlags = 0;
#if defined(_DEBUG)
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    HRESULT hr = ::CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) {
        // Retry without the debug flag (the DXGI debug layer may be absent).
        hr = ::CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
    }
    if (FAILED(hr)) {
        DS_ERR(L"CreateDXGIFactory2(IDXGIFactory5) failed: %s", HrString(hr));
        return false;
    }

    // Tearing support is queried by WindowPresenter, which is the only path
    // that can use it.

    if (!PickAdapter(m_factory.Get(), m_cfg.physicalDevice, m_adapter)) {
        DS_ERR(L"no DXGI adapter available");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad = {};
    m_adapter->GetDesc1(&ad);
    m_gpu.adapterLuid = ad.AdapterLuid;
    DS_LOG(L"render adapter: %s", ad.Description);

    UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    hr = ::D3D11CreateDevice(m_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                             deviceFlags, levels, _countof(levels),
                             D3D11_SDK_VERSION, &m_gpu.device, &got, &m_gpu.ctx);
#if defined(_DEBUG)
    if (FAILED(hr)) {
        deviceFlags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = ::D3D11CreateDevice(m_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 deviceFlags, levels, _countof(levels),
                                 D3D11_SDK_VERSION, &m_gpu.device, &got, &m_gpu.ctx);
    }
#endif
    if (FAILED(hr)) {
        DS_ERR(L"D3D11CreateDevice failed: %s", HrString(hr));
        return false;
    }

    // The same device is handed to IDXGIOutput1::DuplicateOutput on the capture
    // threads, and the duplication object drives the immediate context
    // internally (AcquireNextFrame / ReleaseFrame / GetFramePointerShape). Our
    // own mutex cannot cover those internal uses, so let the runtime serialise
    // context access as well. Without this the driver faults under load.
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(m_gpu.ctx.As(&multithread)) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
        DS_VERB(L"D3D11 multithread protection enabled");
    } else {
        DS_WARN(L"ID3D11Multithread unavailable; capture threads may race");
    }

    return true;
}

// Chooses between the specialized display path and the window swapchain, and
// falls back to the window with a logged reason whenever the specialized path
// is not fully usable. The decision policy itself lives in PresenterSelect.h so
// it can be unit-tested against mocked probe results.
bool Renderer::CreatePresenter(HWND hwnd, bool wantSpecialized) {
    PresenterInit init;
    init.hwnd = hwnd;
    init.cfg = &m_cfg;
    init.device = m_gpu.device.Get();
    init.ctx = m_gpu.ctx.Get();
    init.factory = m_factory.Get();
    init.adapter = m_adapter.Get();
    init.ctxMutex = &m_gpu.mtx;

    SpecializedAvailability avail;
    avail.requested = wantSpecialized;

    std::unique_ptr<SpecializedPresenter> specialized;
    if (wantSpecialized) {
        specialized = std::make_unique<SpecializedPresenter>();
        if (!specialized->TryAcquire(init, avail)) {
            specialized->Shutdown();
            specialized.reset();
        }
    }

    m_selection = ChoosePresenter(avail);
    if (m_selection.kind == PresenterKind::Specialized && specialized) {
        m_presenter = std::move(specialized);
        if (m_presenter->Initialize(init)) {
            DS_LOG(L"presenter: SPECIALIZED - %s", m_selection.reason.c_str());
            return true;
        }
        m_presenter->Shutdown();
        m_presenter.reset();
        m_selection.kind = PresenterKind::Window;
        m_selection.fellBack = true;
        m_selection.reason = L"specialized presenter failed to initialise";
    }

    if (m_selection.fellBack) {
        DS_WARN(L"presenter: falling back to the window path - %s",
                m_selection.reason.c_str());
    } else {
        DS_LOG(L"presenter: window - %s", m_selection.reason.c_str());
    }
    m_presenter = std::make_unique<WindowPresenter>();
    return m_presenter->Initialize(init);
}

bool Renderer::CreatePipeline() {
    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!CompileShader("VSMain", "vs_4_0", vsBlob)) return false;
    if (!CompileShader("PSMain", "ps_4_0", psBlob)) return false;

    HRESULT hr = m_gpu.device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                                  vsBlob->GetBufferSize(), nullptr, &m_vs);
    if (FAILED(hr)) { DS_ERR(L"CreateVertexShader failed: %s", HrString(hr)); return false; }
    hr = m_gpu.device->CreatePixelShader(psBlob->GetBufferPointer(),
                                          psBlob->GetBufferSize(), nullptr, &m_ps);
    if (FAILED(hr)) { DS_ERR(L"CreatePixelShader failed: %s", HrString(hr)); return false; }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(QuadCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_gpu.device->CreateBuffer(&bd, nullptr, &m_cb);
    if (FAILED(hr)) { DS_ERR(L"CreateBuffer(cb) failed: %s", HrString(hr)); return false; }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_gpu.device->CreateSamplerState(&sd, &m_sampler);
    if (FAILED(hr)) { DS_ERR(L"CreateSamplerState failed: %s", HrString(hr)); return false; }

    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = FALSE;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = m_gpu.device->CreateBlendState(&bl, &m_blendOpaque);
    if (FAILED(hr)) { DS_ERR(L"CreateBlendState(opaque) failed: %s", HrString(hr)); return false; }

    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    hr = m_gpu.device->CreateBlendState(&bl, &m_blendAlpha);
    if (FAILED(hr)) { DS_ERR(L"CreateBlendState(alpha) failed: %s", HrString(hr)); return false; }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    hr = m_gpu.device->CreateRasterizerState(&rd, &m_raster);
    if (FAILED(hr)) { DS_ERR(L"CreateRasterizerState failed: %s", HrString(hr)); return false; }

    return true;
}

void Renderer::RequestResize(UINT width, UINT height) {
    if (m_presenter) m_presenter->RequestResize(width, height);
}

void Renderer::DrawQuad(ID3D11ShaderResourceView* srv, float px, float py,
                        float pw, float ph) {
    QuadCB cb = {};
    cb.pos[0] = px;
    cb.pos[1] = py;
    cb.pos[2] = pw;
    cb.pos[3] = ph;
    cb.uv[0] = 0.0f;
    cb.uv[1] = 0.0f;
    cb.uv[2] = 1.0f;
    cb.uv[3] = 1.0f;

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(m_gpu.ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    memcpy(map.pData, &cb, sizeof(cb));
    m_gpu.ctx->Unmap(m_cb.Get(), 0);

    ID3D11Buffer* cbs[] = { m_cb.Get() };
    m_gpu.ctx->VSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[] = { srv };
    m_gpu.ctx->PSSetShaderResources(0, 1, srvs);
    m_gpu.ctx->Draw(4, 0);
}

// Render-loop phase timing, reported on the --verbose stats line.
//
// Worth keeping: a frame-rate collapse in this pipeline can come from the
// present path, from GPU-lock contention, or from the D3D11 device lock, and
// those are indistinguishable from an fps number alone. Splitting them is what
// identified the AcquireNextFrame device-lock stall documented in Capture.cpp.
// Cost is four QueryPerformanceCounter calls per frame.
namespace {
std::atomic<long long> g_diagPresentTicks{0};   // time inside Present
std::atomic<long long> g_diagGpuTicks{0};       // GPU-lock block: wait + draws
std::atomic<long long> g_diagLockTicks{0};      // of which: waiting for m_gpu.mtx
std::atomic<long>      g_diagPresentHr{0};      // last Present HRESULT (occlusion shows here)
}

void Renderer::RenderFrame() {
    if (!m_presenter) return;
    ID3D11RenderTargetView* rtv = m_presenter->BeginFrame();
    if (!rtv) return;
    const UINT frameW = m_presenter->Width();
    const UINT frameH = m_presenter->Height();
    if (frameW == 0 || frameH == 0) return;

    const size_t segCount = m_cfg.segments.size();

    // Snapshot per-segment state without holding the GPU lock.
    struct Snapshot {
        ComPtr<ID3D11ShaderResourceView> srv;
        UINT frameW = 0, frameH = 0;
        bool hasFrame = false;
        bool cursorVisible = false;
        int  cursorX = 0, cursorY = 0;
        UINT cursorW = 0, cursorH = 0;
        uint64_t cursorVersion = 0;
        std::vector<uint8_t> cursorPixels;   // only filled when the shape changed
    };
    std::vector<Snapshot> snaps(segCount);

    for (size_t i = 0; i < segCount; ++i) {
        SegmentShared& s = (*m_segments)[i];
        Snapshot& snap = snaps[i];
        std::lock_guard<std::mutex> lock(s.mtx);
        snap.srv = s.frameSrv;
        snap.frameW = s.frameWidth;
        snap.frameH = s.frameHeight;
        snap.hasFrame = s.hasFrame;
        snap.cursorVisible = s.cursorVisible;
        snap.cursorX = s.cursorX;
        snap.cursorY = s.cursorY;
        snap.cursorW = s.cursorWidth;
        snap.cursorH = s.cursorHeight;
        snap.cursorVersion = s.cursorShapeVersion;
        if (snap.cursorVersion != m_cursors[i].version && !s.cursorBgra.empty()) {
            snap.cursorPixels = s.cursorBgra;
        }
    }

    // Refresh cursor textures whose shape changed (immutable, no context needed).
    for (size_t i = 0; i < segCount; ++i) {
        Snapshot& snap = snaps[i];
        CursorGpu& cg = m_cursors[i];
        if (snap.cursorPixels.empty() || snap.cursorW == 0 || snap.cursorH == 0) continue;
        if (snap.cursorPixels.size() <
            static_cast<size_t>(snap.cursorW) * snap.cursorH * 4) {
            continue;
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = snap.cursorW;
        td.Height = snap.cursorH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init = {};
        init.pSysMem = snap.cursorPixels.data();
        init.SysMemPitch = snap.cursorW * 4;

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(m_gpu.device->CreateTexture2D(&td, &init, &tex))) continue;
        ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(m_gpu.device->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
            continue;
        cg.tex = tex;
        cg.srv = srv;
        cg.width = snap.cursorW;
        cg.height = snap.cursorH;
        cg.version = snap.cursorVersion;
    }

    const float fw = static_cast<float>(frameW);
    const float fh = static_cast<float>(frameH);

    LARGE_INTEGER gt0, gtLocked, gt1;
    ::QueryPerformanceCounter(&gt0);
    {
        std::lock_guard<std::mutex> lock(m_gpu.mtx);
        ::QueryPerformanceCounter(&gtLocked);
        g_diagLockTicks.fetch_add(gtLocked.QuadPart - gt0.QuadPart, std::memory_order_relaxed);

        ID3D11RenderTargetView* rtvs[] = { rtv };
        m_gpu.ctx->OMSetRenderTargets(1, rtvs, nullptr);

        D3D11_VIEWPORT vp = {};
        vp.Width = fw;
        vp.Height = fh;
        vp.MaxDepth = 1.0f;
        m_gpu.ctx->RSSetViewports(1, &vp);
        m_gpu.ctx->RSSetState(m_raster.Get());

        const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        D3D11_RECT full = { 0, 0, static_cast<LONG>(frameW), static_cast<LONG>(frameH) };
        m_gpu.ctx->RSSetScissorRects(1, &full);
        m_gpu.ctx->ClearRenderTargetView(rtv, clear);

        m_gpu.ctx->IASetInputLayout(nullptr);
        m_gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_gpu.ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        m_gpu.ctx->PSSetShader(m_ps.Get(), nullptr, 0);
        ID3D11SamplerState* samplers[] = { m_sampler.Get() };
        m_gpu.ctx->PSSetSamplers(0, 1, samplers);

        for (size_t i = 0; i < segCount; ++i) {
            const SegmentConfig& sc = m_cfg.segments[i];
            const Snapshot& snap = snaps[i];
            if (!snap.hasFrame || !snap.srv || snap.frameW == 0 || snap.frameH == 0)
                continue;

            const float rx = static_cast<float>(sc.physRect.x);
            const float ry = static_cast<float>(sc.physRect.y);
            const float rw = static_cast<float>(sc.physRect.w);
            const float rh = static_cast<float>(sc.physRect.h);

            D3D11_RECT scissor;
            scissor.left   = std::max<LONG>(0, static_cast<LONG>(sc.physRect.x));
            scissor.top    = std::max<LONG>(0, static_cast<LONG>(sc.physRect.y));
            scissor.right  = std::min<LONG>(static_cast<LONG>(frameW),
                                            static_cast<LONG>(sc.physRect.x + sc.physRect.w));
            scissor.bottom = std::min<LONG>(static_cast<LONG>(frameH),
                                            static_cast<LONG>(sc.physRect.y + sc.physRect.h));
            if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
            m_gpu.ctx->RSSetScissorRects(1, &scissor);

            const float ndcX = (rx / fw) * 2.0f - 1.0f;
            const float ndcY = 1.0f - (ry / fh) * 2.0f;
            const float ndcW = (rw / fw) * 2.0f;
            const float ndcH = (rh / fh) * 2.0f;

            m_gpu.ctx->OMSetBlendState(m_blendOpaque.Get(), nullptr, 0xFFFFFFFF);
            DrawQuad(snap.srv.Get(), ndcX, ndcY, ndcW, ndcH);

            // Cursor, scaled with the segment.
            const CursorGpu& cg = m_cursors[i];
            if (snap.cursorVisible && cg.srv && cg.width > 0 && cg.height > 0) {
                const float scaleX = rw / static_cast<float>(snap.frameW);
                const float scaleY = rh / static_cast<float>(snap.frameH);
                const float cx = rx + static_cast<float>(snap.cursorX) * scaleX;
                const float cy = ry + static_cast<float>(snap.cursorY) * scaleY;
                const float cw = static_cast<float>(cg.width) * scaleX;
                const float ch = static_cast<float>(cg.height) * scaleY;

                const float cNdcX = (cx / fw) * 2.0f - 1.0f;
                const float cNdcY = 1.0f - (cy / fh) * 2.0f;
                const float cNdcW = (cw / fw) * 2.0f;
                const float cNdcH = (ch / fh) * 2.0f;

                m_gpu.ctx->OMSetBlendState(m_blendAlpha.Get(), nullptr, 0xFFFFFFFF);
                DrawQuad(cg.srv.Get(), cNdcX, cNdcY, cNdcW, cNdcH);
            }
        }

        // Unbind SRVs so the next capture CopyResource never hits a bound resource.
        ID3D11ShaderResourceView* nullSrv[] = { nullptr };
        m_gpu.ctx->PSSetShaderResources(0, 1, nullSrv);
        m_gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
    ::QueryPerformanceCounter(&gt1);
    g_diagGpuTicks.fetch_add(gt1.QuadPart - gt0.QuadPart, std::memory_order_relaxed);

    HRESULT hr;
    LARGE_INTEGER pt0, pt1;
    ::QueryPerformanceCounter(&pt0);
    hr = m_presenter->EndFrame();
    ::QueryPerformanceCounter(&pt1);
    g_diagPresentTicks.fetch_add(pt1.QuadPart - pt0.QuadPart, std::memory_order_relaxed);
    g_diagPresentHr.store(static_cast<long>(hr), std::memory_order_relaxed);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        DS_ERR(L"device removed during Present: %s",
               HrString(m_gpu.device->GetDeviceRemovedReason()));
        m_stop.store(true, std::memory_order_release);
    } else if (FAILED(hr)) {
        DS_WARN(L"Present failed: %s", HrString(hr));
    }
}

void Renderer::PaceFrame() {
    // A self-paced presenter (vsync Present, or scanout with a sync interval)
    // already blocks for the display; pacing on top would only add latency.
    if (!m_presenter || m_presenter->SelfPaced() || m_qpcFreq.QuadPart == 0) return;

    const double hz = static_cast<double>(m_cfg.refreshMillihertz) / 1000.0;
    if (hz < 1.0) return;
    const long long period =
        static_cast<long long>(static_cast<double>(m_qpcFreq.QuadPart) / hz);
    if (period <= 0) return;

    LARGE_INTEGER now = {};
    ::QueryPerformanceCounter(&now);
    if (m_nextPresentQpc == 0) m_nextPresentQpc = now.QuadPart;
    m_nextPresentQpc += period;
    if (m_nextPresentQpc < now.QuadPart) {
        // Fell behind: skip whole periods so the cadence keeps its phase.
        const long long behind = now.QuadPart - m_nextPresentQpc;
        m_nextPresentQpc += ((behind / period) + 1) * period;
        return;
    }

    const long long waitTicks = m_nextPresentQpc - now.QuadPart;
    if (waitTicks <= 0) return;
    const long long wait100ns = waitTicks * 10000000LL / m_qpcFreq.QuadPart;
    if (wait100ns <= 0) return;

    if (m_paceTimer) {
        LARGE_INTEGER due;
        due.QuadPart = -wait100ns;
        if (::SetWaitableTimer(m_paceTimer, &due, 0, nullptr, nullptr, FALSE)) {
            ::WaitForSingleObject(m_paceTimer, static_cast<DWORD>(
                                                   (wait100ns / 10000) + 2));
            return;
        }
    }
    ::Sleep(static_cast<DWORD>(wait100ns / 10000));
}

void Renderer::ThreadMain() {
    ::SetThreadDescription(::GetCurrentThread(), L"dscomp-render");

    DWORD    statsTick = ::GetTickCount();
    uint64_t presented = 0;
    std::vector<uint64_t> lastCaptured(m_segments ? m_segments->size() : 0, 0);

    while (!m_stop.load(std::memory_order_acquire)) {
        RenderFrame();
        ++presented;
        PaceFrame();

        if (LogIsVerbose()) {
            const DWORD now = ::GetTickCount();
            const DWORD elapsed = now - statsTick;
            if (elapsed >= 2000) {
                wchar_t caps[256] = L"";
                for (size_t i = 0; i < lastCaptured.size(); ++i) {
                    const uint64_t total =
                        (*m_segments)[i].framesCaptured.load(std::memory_order_relaxed);
                    wchar_t part[64];
                    _snwprintf_s(part, _countof(part), _TRUNCATE, L" seg%zu=%.1f", i,
                                 static_cast<double>(total - lastCaptured[i]) * 1000.0 /
                                     static_cast<double>(elapsed));
                    ::wcscat_s(caps, part);
                    lastCaptured[i] = total;
                }
                const long long pTicks = g_diagPresentTicks.exchange(0, std::memory_order_relaxed);
                const long long gTicks = g_diagGpuTicks.exchange(0, std::memory_order_relaxed);
                const double tickMs = (m_qpcFreq.QuadPart > 0)
                                          ? 1000.0 / static_cast<double>(m_qpcFreq.QuadPart)
                                          : 0.0;
                const long long lTicks = g_diagLockTicks.exchange(0, std::memory_order_relaxed);
                DS_VERB(L"present=%.1f fps capture fps:%s | per-frame: present=%.2fms gpu=%.2fms "
                        L"(lockwait=%.2fms draw=%.2fms) other=%.2fms lastPresentHr=0x%08X",
                        static_cast<double>(presented) * 1000.0 /
                            static_cast<double>(elapsed),
                        caps,
                        presented ? static_cast<double>(pTicks) * tickMs / static_cast<double>(presented) : 0.0,
                        presented ? static_cast<double>(gTicks) * tickMs / static_cast<double>(presented) : 0.0,
                        presented ? static_cast<double>(lTicks) * tickMs / static_cast<double>(presented) : 0.0,
                        presented ? static_cast<double>(gTicks - lTicks) * tickMs / static_cast<double>(presented) : 0.0,
                        presented ? (static_cast<double>(elapsed) / static_cast<double>(presented)) -
                                        (static_cast<double>(pTicks + gTicks) * tickMs /
                                         static_cast<double>(presented))
                                  : 0.0,
                        static_cast<unsigned>(g_diagPresentHr.load(std::memory_order_relaxed)));
                presented = 0;
                statsTick = now;
            }
        }
    }
    DS_VERB(L"render thread exiting");
}

bool Renderer::StartThread() {
    m_stop.store(false, std::memory_order_release);
    m_thread = std::thread([this] { ThreadMain(); });
    return true;
}

void Renderer::StopThread() {
    m_stop.store(true, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
}

void Renderer::Shutdown() {
    m_cursors.clear();
    if (m_presenter) {
        m_presenter->Shutdown();
        m_presenter.reset();
    }
    m_raster.Reset();
    m_blendAlpha.Reset();
    m_blendOpaque.Reset();
    m_sampler.Reset();
    m_cb.Reset();
    m_ps.Reset();
    m_vs.Reset();
    if (m_gpu.ctx) {
        m_gpu.ctx->ClearState();
        m_gpu.ctx->Flush();
    }
    m_gpu.ctx.Reset();
    m_gpu.device.Reset();
    m_adapter.Reset();
    m_factory.Reset();
    if (m_paceTimer) {
        ::CloseHandle(m_paceTimer);
        m_paceTimer = nullptr;
    }
}

} // namespace ds
