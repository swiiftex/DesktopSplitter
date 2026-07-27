// Capture.h - one DXGI Desktop Duplication thread per segment.
#pragma once

#include "Common.h"
#include "SegmentShared.h"

#include <dxgi1_5.h>
#include <thread>

namespace ds {

class SegmentCapture {
public:
    SegmentCapture() = default;
    ~SegmentCapture();

    SegmentCapture(const SegmentCapture&) = delete;
    SegmentCapture& operator=(const SegmentCapture&) = delete;

    // Starts the capture thread. Never fails hard: if the virtual monitor is
    // not present yet the thread keeps retrying until Stop().
    bool Start(size_t index, const SegmentConfig& cfg, GpuContext* gpu,
               SegmentShared* shared);
    void Stop();

private:
    void ThreadMain();
    bool InitDuplication();
    void TeardownDuplication();
    bool PublishFrame(ID3D11Texture2D* acquired);
    bool EnsureSharedTexture(UINT width, UINT height);
    void UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info);

    size_t          m_index = 0;
    SegmentConfig   m_cfg;
    GpuContext*     m_gpu = nullptr;
    SegmentShared*  m_shared = nullptr;

    std::thread     m_thread;
    HANDLE          m_stopEvent = nullptr;

    // Duplication state (owned by the capture thread).
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_dupl;
    Microsoft::WRL::ComPtr<ID3D11Device>           m_capDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_capCtx;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_staging;   // cross-adapter only
    UINT                                           m_stagingW = 0;
    UINT                                           m_stagingH = 0;
    bool                                           m_crossAdapter = false;
    bool                                           m_holdingFrame = false;

    std::vector<uint8_t> m_shapeBuffer;   // raw pointer shape from DXGI
    std::vector<uint8_t> m_shapeBgra;     // converted BGRA
};

} // namespace ds
