#include "Capture.h"
#include "Log.h"

#include <d3d11.h>

using Microsoft::WRL::ComPtr;

namespace ds {

namespace {

// AcquireNextFrame must NEVER block: every capture thread shares the renderer's
// D3D11 device, and that device has multithread protection enabled, so a
// blocking AcquireNextFrame holds the device-wide lock for its whole timeout and
// starves the render thread's draw calls.
//
// Measured on a 3-segment split with an idle desktop and a 4 ms timeout: the
// render thread's draw block took 260-417 ms per frame (lock wait inside D3D11,
// not our own mutex), i.e. ~3 fps. It went smooth whenever something forced
// continuous composition - e.g. opening the Start menu - because then
// AcquireNextFrame returned immediately instead of burning its timeout.
//
// With a 0 ms timeout the call returns at once and the device lock is held only
// for the moment it takes to hand over a frame; idle polling is paced by a short
// wait on the stop event instead (which also makes shutdown more responsive).
constexpr UINT kAcquireTimeoutMs = 0;
constexpr DWORD kIdlePollMs = 1;
constexpr DWORD kRetryDelayMs = 250;

// Converts a DXGI pointer shape to tightly packed BGRA. Returns false when the
// shape type is unknown or the buffer is too small.
bool ConvertPointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info,
                         const std::vector<uint8_t>& raw,
                         std::vector<uint8_t>& outBgra,
                         UINT& outWidth, UINT& outHeight) {
    const UINT pitch = info.Pitch;
    if (pitch == 0 || info.Width == 0 || info.Height == 0) return false;

    switch (info.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR: {
        outWidth = info.Width;
        outHeight = info.Height;
        if (raw.size() < static_cast<size_t>(pitch) * outHeight) return false;
        outBgra.assign(static_cast<size_t>(outWidth) * outHeight * 4, 0);
        for (UINT y = 0; y < outHeight; ++y) {
            memcpy(&outBgra[static_cast<size_t>(y) * outWidth * 4],
                   &raw[static_cast<size_t>(y) * pitch],
                   static_cast<size_t>(outWidth) * 4);
        }
        return true;
    }

    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME: {
        // Top half = AND mask, bottom half = XOR mask, 1 bit per pixel.
        outWidth = info.Width;
        outHeight = info.Height / 2;
        if (outHeight == 0) return false;
        if (raw.size() < static_cast<size_t>(pitch) * info.Height) return false;
        outBgra.assign(static_cast<size_t>(outWidth) * outHeight * 4, 0);
        for (UINT y = 0; y < outHeight; ++y) {
            const uint8_t* andRow = &raw[static_cast<size_t>(y) * pitch];
            const uint8_t* xorRow = &raw[static_cast<size_t>(y + outHeight) * pitch];
            for (UINT x = 0; x < outWidth; ++x) {
                const uint8_t bit = static_cast<uint8_t>(0x80 >> (x & 7));
                const bool a = (andRow[x >> 3] & bit) != 0;
                const bool b = (xorRow[x >> 3] & bit) != 0;
                uint8_t* px = &outBgra[(static_cast<size_t>(y) * outWidth + x) * 4];
                if (!a && !b) {           // opaque black
                    px[0] = px[1] = px[2] = 0x00; px[3] = 0xFF;
                } else if (!a && b) {     // opaque white
                    px[0] = px[1] = px[2] = 0xFF; px[3] = 0xFF;
                } else if (a && !b) {     // transparent
                    px[0] = px[1] = px[2] = px[3] = 0x00;
                } else {                  // screen invert - approximated as white
                    px[0] = px[1] = px[2] = 0xFF; px[3] = 0xFF;
                }
            }
        }
        return true;
    }

    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: {
        // 32bpp; the alpha byte is a mask: 0 = replace screen, 0xFF = XOR.
        outWidth = info.Width;
        outHeight = info.Height;
        if (raw.size() < static_cast<size_t>(pitch) * outHeight) return false;
        outBgra.assign(static_cast<size_t>(outWidth) * outHeight * 4, 0);
        for (UINT y = 0; y < outHeight; ++y) {
            const uint8_t* src = &raw[static_cast<size_t>(y) * pitch];
            for (UINT x = 0; x < outWidth; ++x) {
                const uint8_t* s = src + static_cast<size_t>(x) * 4;
                uint8_t* px = &outBgra[(static_cast<size_t>(y) * outWidth + x) * 4];
                if (s[3] == 0) {
                    px[0] = s[0]; px[1] = s[1]; px[2] = s[2]; px[3] = 0xFF;
                } else {
                    // XOR-with-screen is approximated by inverting the colour.
                    px[0] = static_cast<uint8_t>(~s[0]);
                    px[1] = static_cast<uint8_t>(~s[1]);
                    px[2] = static_cast<uint8_t>(~s[2]);
                    px[3] = 0xFF;
                }
            }
        }
        return true;
    }

    default:
        return false;
    }
}

// Finds the DXGI output whose DeviceName matches, plus its adapter.
bool ResolveOutput(const std::wstring& deviceName, ComPtr<IDXGIAdapter1>& adapter,
                   ComPtr<IDXGIOutput1>& output1, LUID& adapterLuid) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        DS_ERR(L"CreateDXGIFactory1 failed: %s", HrString(hr));
        return false;
    }
    for (UINT ai = 0;; ++ai) {
        ComPtr<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(ai, &a) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT oi = 0;; ++oi) {
            ComPtr<IDXGIOutput> o;
            if (a->EnumOutputs(oi, &o) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od = {};
            if (FAILED(o->GetDesc(&od))) continue;
            if (::_wcsicmp(od.DeviceName, deviceName.c_str()) != 0) continue;

            ComPtr<IDXGIOutput1> o1;
            if (FAILED(o.As(&o1))) {
                DS_ERR(L"output '%s' does not support IDXGIOutput1", deviceName.c_str());
                return false;
            }
            DXGI_ADAPTER_DESC1 ad = {};
            a->GetDesc1(&ad);
            adapter = a;
            output1 = o1;
            adapterLuid = ad.AdapterLuid;
            return true;
        }
    }
    return false;
}

bool SameLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

} // namespace

SegmentCapture::~SegmentCapture() {
    Stop();
    if (m_stopEvent) {
        ::CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

bool SegmentCapture::Start(size_t index, const SegmentConfig& cfg, GpuContext* gpu,
                           SegmentShared* shared) {
    m_index = index;
    m_cfg = cfg;
    m_gpu = gpu;
    m_shared = shared;

    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) {
        DS_ERR(L"segment %zu: CreateEvent failed", index);
        return false;
    }
    m_thread = std::thread([this] { ThreadMain(); });
    return true;
}

void SegmentCapture::Stop() {
    if (m_stopEvent) ::SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
}

void SegmentCapture::TeardownDuplication() {
    if (m_dupl && m_holdingFrame) {
        m_dupl->ReleaseFrame();
        m_holdingFrame = false;
    }
    m_dupl.Reset();
    m_staging.Reset();
    m_stagingW = m_stagingH = 0;
    if (m_crossAdapter) {
        m_capCtx.Reset();
        m_capDevice.Reset();
    }
}

bool SegmentCapture::InitDuplication() {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1>  output1;
    LUID                  outLuid = {};

    if (!ResolveOutput(m_cfg.virtualDevice, adapter, output1, outLuid)) {
        DS_VERB(L"segment %zu: output '%s' not found yet", m_index,
                m_cfg.virtualDevice.c_str());
        return false;
    }

    m_crossAdapter = !SameLuid(outLuid, m_gpu->adapterLuid);
    if (m_crossAdapter) {
        // The duplication device must live on the output's adapter; frames are
        // moved to the render device through a CPU staging copy.
        static const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
        };
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        const HRESULT hr = ::D3D11CreateDevice(
            adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, _countof(levels),
            D3D11_SDK_VERSION, &m_capDevice, &got, &m_capCtx);
        if (FAILED(hr)) {
            DS_ERR(L"segment %zu: capture device creation failed: %s", m_index,
                   HrString(hr));
            return false;
        }
        DS_LOG(L"segment %zu: '%s' is on a different adapter - using CPU staging path",
               m_index, m_cfg.virtualDevice.c_str());
    } else {
        m_capDevice = m_gpu->device;
        m_capCtx = m_gpu->ctx;
    }

    const HRESULT hr = output1->DuplicateOutput(m_capDevice.Get(), &m_dupl);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            DS_WARN(L"segment %zu: duplication unavailable (too many duplications?)",
                    m_index);
        } else {
            DS_WARN(L"segment %zu: DuplicateOutput failed: %s", m_index, HrString(hr));
        }
        TeardownDuplication();
        return false;
    }

    DXGI_OUTDUPL_DESC dd = {};
    m_dupl->GetDesc(&dd);
    DS_LOG(L"segment %zu: duplicating '%s' %ux%u fmt=%u", m_index,
           m_cfg.virtualDevice.c_str(), dd.ModeDesc.Width, dd.ModeDesc.Height,
           static_cast<unsigned>(dd.ModeDesc.Format));
    return true;
}

bool SegmentCapture::EnsureSharedTexture(UINT width, UINT height) {
    {
        std::lock_guard<std::mutex> lock(m_shared->mtx);
        if (m_shared->frameTex && m_shared->frameWidth == width &&
            m_shared->frameHeight == height) {
            return true;
        }
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (m_crossAdapter) {
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    } else {
        td.Usage = D3D11_USAGE_DEFAULT;
    }

    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = m_gpu->device->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr)) {
        DS_ERR(L"segment %zu: CreateTexture2D(%ux%u) failed: %s", m_index, width,
               height, HrString(hr));
        return false;
    }
    ComPtr<ID3D11ShaderResourceView> srv;
    hr = m_gpu->device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    if (FAILED(hr)) {
        DS_ERR(L"segment %zu: CreateShaderResourceView failed: %s", m_index,
               HrString(hr));
        return false;
    }

    std::lock_guard<std::mutex> lock(m_shared->mtx);
    m_shared->frameTex = tex;
    m_shared->frameSrv = srv;
    m_shared->frameWidth = width;
    m_shared->frameHeight = height;
    m_shared->hasFrame = false;
    DS_VERB(L"segment %zu: shared texture is now %ux%u", m_index, width, height);
    return true;
}

bool SegmentCapture::PublishFrame(ID3D11Texture2D* acquired) {
    D3D11_TEXTURE2D_DESC sd = {};
    acquired->GetDesc(&sd);
    if (!EnsureSharedTexture(sd.Width, sd.Height)) return false;

    ComPtr<ID3D11Texture2D> dest;
    {
        std::lock_guard<std::mutex> lock(m_shared->mtx);
        dest = m_shared->frameTex;
    }
    if (!dest) return false;

    if (!m_crossAdapter) {
        std::lock_guard<std::mutex> lock(m_gpu->mtx);
        m_gpu->ctx->CopyResource(dest.Get(), acquired);
    } else {
        if (!m_staging || m_stagingW != sd.Width || m_stagingH != sd.Height) {
            D3D11_TEXTURE2D_DESC td = sd;
            td.Usage = D3D11_USAGE_STAGING;
            td.BindFlags = 0;
            td.MiscFlags = 0;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            m_staging.Reset();
            const HRESULT hr = m_capDevice->CreateTexture2D(&td, nullptr, &m_staging);
            if (FAILED(hr)) {
                DS_ERR(L"segment %zu: staging texture failed: %s", m_index, HrString(hr));
                return false;
            }
            m_stagingW = sd.Width;
            m_stagingH = sd.Height;
        }
        m_capCtx->CopyResource(m_staging.Get(), acquired);

        D3D11_MAPPED_SUBRESOURCE src = {};
        HRESULT hr = m_capCtx->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &src);
        if (FAILED(hr)) {
            DS_WARN(L"segment %zu: staging Map failed: %s", m_index, HrString(hr));
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(m_gpu->mtx);
            D3D11_MAPPED_SUBRESOURCE dst = {};
            hr = m_gpu->ctx->Map(dest.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &dst);
            if (SUCCEEDED(hr)) {
                const size_t rowBytes = static_cast<size_t>(sd.Width) * 4;
                for (UINT y = 0; y < sd.Height; ++y) {
                    memcpy(static_cast<uint8_t*>(dst.pData) + static_cast<size_t>(y) * dst.RowPitch,
                           static_cast<const uint8_t*>(src.pData) + static_cast<size_t>(y) * src.RowPitch,
                           rowBytes);
                }
                m_gpu->ctx->Unmap(dest.Get(), 0);
            }
        }
        m_capCtx->Unmap(m_staging.Get(), 0);
        if (FAILED(hr)) return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_shared->mtx);
        m_shared->hasFrame = true;
    }
    m_shared->framesCaptured.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void SegmentCapture::UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info) {
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        std::lock_guard<std::mutex> lock(m_shared->mtx);
        m_shared->cursorVisible = info.PointerPosition.Visible != FALSE;
        m_shared->cursorX = info.PointerPosition.Position.x;
        m_shared->cursorY = info.PointerPosition.Position.y;
    }

    if (info.PointerShapeBufferSize == 0) return;

    if (m_shapeBuffer.size() < info.PointerShapeBufferSize) {
        m_shapeBuffer.resize(info.PointerShapeBufferSize);
    }
    UINT required = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO si = {};
    const HRESULT hr = m_dupl->GetFramePointerShape(
        static_cast<UINT>(m_shapeBuffer.size()), m_shapeBuffer.data(), &required, &si);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_MORE_DATA && required > m_shapeBuffer.size()) {
            m_shapeBuffer.resize(required);
        } else {
            DS_VERB(L"segment %zu: GetFramePointerShape failed: %s", m_index, HrString(hr));
        }
        return;
    }

    UINT w = 0, h = 0;
    if (!ConvertPointerShape(si, m_shapeBuffer, m_shapeBgra, w, h)) {
        DS_VERB(L"segment %zu: unsupported pointer shape type %u", m_index,
                static_cast<unsigned>(si.Type));
        return;
    }

    std::lock_guard<std::mutex> lock(m_shared->mtx);
    m_shared->cursorWidth = w;
    m_shared->cursorHeight = h;
    m_shared->cursorBgra = m_shapeBgra;
    ++m_shared->cursorShapeVersion;
}

void SegmentCapture::ThreadMain() {
    ::SetThreadDescription(::GetCurrentThread(), L"dscomp-capture");

    bool firstFrame = true;
    for (;;) {
        if (::WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) break;

        if (!m_dupl) {
            if (!InitDuplication()) {
                if (::WaitForSingleObject(m_stopEvent, kRetryDelayMs) == WAIT_OBJECT_0)
                    break;
                continue;
            }
            firstFrame = true;
        }

        DXGI_OUTDUPL_FRAME_INFO info = {};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = m_dupl->AcquireNextFrame(kAcquireTimeoutMs, &info, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // No new frame. Idle here, NOT inside AcquireNextFrame, so the
            // shared D3D11 device lock stays free for the render thread.
            if (::WaitForSingleObject(m_stopEvent, kIdlePollMs) == WAIT_OBJECT_0) break;
            continue;
        }
        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
                DS_VERB(L"segment %zu: duplication access lost, recreating", m_index);
            } else {
                DS_WARN(L"segment %zu: AcquireNextFrame failed: %s", m_index, HrString(hr));
            }
            TeardownDuplication();
            if (::WaitForSingleObject(m_stopEvent, 50) == WAIT_OBJECT_0) break;
            continue;
        }
        m_holdingFrame = true;

        if (info.LastPresentTime.QuadPart != 0 || firstFrame) {
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(resource.As(&tex)) && tex) {
                if (PublishFrame(tex.Get())) firstFrame = false;
            }
        }

        UpdatePointer(info);

        hr = m_dupl->ReleaseFrame();
        m_holdingFrame = false;
        if (FAILED(hr) && hr != DXGI_ERROR_INVALID_CALL) {
            DS_VERB(L"segment %zu: ReleaseFrame failed: %s", m_index, HrString(hr));
            TeardownDuplication();
        }
    }

    TeardownDuplication();
    DS_VERB(L"segment %zu: capture thread exiting", m_index);
}

} // namespace ds
