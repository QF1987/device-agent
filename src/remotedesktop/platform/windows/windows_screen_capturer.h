#pragma once

#ifdef _WIN32

#include "remotedesktop/screen_capturer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <windows.h>

namespace device_agent::remotedesktop::windows {

class WindowsScreenCapturer : public IScreenCapturer {
public:
    bool capture(ScreenFrame& frame, std::string& err) override;

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
};

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
