#pragma once

#ifdef _WIN32

#include "remotedesktop/screen_capturer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <windows.h>

#include <chrono>

namespace device_agent::remotedesktop::windows {

class WindowsScreenCapturer : public IScreenCapturer {
public:
    WindowsScreenCapturer();

    bool capture(ScreenFrame& frame, std::string& err) override;
    bool forceGdi() const { return force_gdi_; }
    uint32_t gdiMaxFps() const { return gdi_max_fps_; }
    uint16_t gdiTileSize() const { return gdi_tile_size_; }
    const char* lastCaptureMode() const { return last_capture_used_gdi_ ? "gdi" : "dxgi"; }

private:
    bool captureWithDxgi(ScreenFrame& frame, std::string& err);
    bool captureWithGdi(ScreenFrame& frame, std::string& err);
    bool initializeDxgi(std::string& err);
    void resetDxgi();

    bool dxgi_initialized_ = false;
    Microsoft::WRL::ComPtr<ID3D11Device> d3d_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    ScreenFrame last_frame_;
    ScreenFrame last_gdi_frame_;
    std::chrono::steady_clock::time_point last_gdi_capture_at_{};
    bool force_gdi_ = false;
    bool last_capture_used_gdi_ = false;
    uint32_t gdi_max_fps_ = 15;
    uint16_t gdi_tile_size_ = 32;
};

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
