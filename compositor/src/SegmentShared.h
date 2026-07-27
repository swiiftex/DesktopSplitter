// SegmentShared.h - state handed from a capture thread to the render thread.
#pragma once

#include "Common.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <mutex>
#include <atomic>

namespace ds {

// Everything the render thread needs from one capture thread.
//
// Threading: 'mtx' guards the members below it. Capture threads publish under
// the lock; the render thread copies out under the lock and never holds it
// while issuing GPU work. GPU immediate-context access is serialised by a
// separate mutex (GpuContext::mtx) and the two are never nested.
struct SegmentShared {
    std::mutex mtx;

    // Latest frame. On the same-adapter path this is a DEFAULT texture that
    // capture fills with CopyResource. On the cross-adapter path it is a
    // DYNAMIC texture that capture fills with Map/memcpy.
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          frameTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> frameSrv;
    UINT frameWidth  = 0;
    UINT frameHeight = 0;
    bool hasFrame    = false;

    // Cursor, in the captured output's local pixel space.
    bool                 cursorVisible = false;
    int                  cursorX = 0;
    int                  cursorY = 0;
    UINT                 cursorWidth = 0;
    UINT                 cursorHeight = 0;
    std::vector<uint8_t> cursorBgra;         // cursorWidth * cursorHeight * 4
    uint64_t             cursorShapeVersion = 0;  // bumped on every shape change

    // Diagnostics.
    std::atomic<uint64_t> framesCaptured{0};
};

// The single D3D11 device shared by the renderer and (normally) all captures.
struct GpuContext {
    Microsoft::WRL::ComPtr<ID3D11Device>        device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    std::mutex                                  mtx;    // guards 'ctx'
    LUID                                        adapterLuid = {};
};

} // namespace ds
