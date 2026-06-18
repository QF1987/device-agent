#ifdef _WIN32

#include "remotedesktop/platform/windows/windows_screen_capturer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace device_agent::remotedesktop::windows {

namespace {

void flushDwmComposition() {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) {
        return;
    }
    using DwmFlushFn = HRESULT(WINAPI*)();
    auto fn = reinterpret_cast<DwmFlushFn>(GetProcAddress(dwm, "DwmFlush"));
    if (fn) {
        (void)fn();
    }
    FreeLibrary(dwm);
}

std::string hrError(const char* op, HRESULT hr) {
    char buf[128]{};
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lx", op, static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace

bool WindowsScreenCapturer::capture(ScreenFrame& frame, std::string& err) {
    std::string dxgi_err;
    if (captureWithDxgi(frame, dxgi_err)) {
        return true;
    }
    // Win7 and session-0/service contexts cannot rely on Desktop Duplication.
    // Keep GDI as the mandatory fallback path for compatibility and diagnostics.
    (void)dxgi_err;
    if (captureWithGdi(frame, err)) {
        return true;
    }
    if (!last_frame_.bgra.empty()) {
        frame = last_frame_;
        err.clear();
        return true;
    }
    return false;
}

bool WindowsScreenCapturer::initializeDxgi(std::string& err) {
    if (dxgi_initialized_) {
        return duplication_ != nullptr;
    }
    dxgi_initialized_ = true;

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected{};
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &d3d_device_,
        &selected,
        &d3d_context_);
    if (FAILED(hr)) {
        err = hrError("D3D11CreateDevice", hr);
        resetDxgi();
        dxgi_initialized_ = true;
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    hr = d3d_device_.As(&dxgi_device);
    if (FAILED(hr)) {
        err = hrError("Query IDXGIDevice", hr);
        resetDxgi();
        dxgi_initialized_ = true;
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        err = hrError("GetAdapter", hr);
        resetDxgi();
        dxgi_initialized_ = true;
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(0, &output);
    if (FAILED(hr)) {
        err = hrError("EnumOutputs", hr);
        resetDxgi();
        dxgi_initialized_ = true;
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        err = hrError("Query IDXGIOutput1", hr);
        resetDxgi();
        dxgi_initialized_ = true;
        return false;
    }

    hr = output1->DuplicateOutput(d3d_device_.Get(), &duplication_);
    if (FAILED(hr)) {
        err = hrError("DuplicateOutput", hr);
        resetDxgi();
        dxgi_initialized_ = true;
        return false;
    }
    return true;
}

void WindowsScreenCapturer::resetDxgi() {
    duplication_.Reset();
    d3d_context_.Reset();
    d3d_device_.Reset();
    dxgi_initialized_ = false;
}

bool WindowsScreenCapturer::captureWithDxgi(ScreenFrame& frame, std::string& err) {
    if (!initializeDxgi(err)) {
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    HRESULT hr = duplication_->AcquireNextFrame(100, &frame_info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        err = "DXGI frame wait timed out";
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
        err = hrError("AcquireNextFrame", hr);
        resetDxgi();
        return false;
    }
    if (FAILED(hr)) {
        err = hrError("AcquireNextFrame", hr);
        return false;
    }

    bool acquired = true;
    auto release_frame = [&]() {
        if (acquired && duplication_) {
            duplication_->ReleaseFrame();
            acquired = false;
        }
    };

    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktop_texture;
    hr = resource.As(&desktop_texture);
    if (FAILED(hr)) {
        err = hrError("Query ID3D11Texture2D", hr);
        release_frame();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desktop_texture->GetDesc(&desc);
    const uint32_t width = std::min<uint32_t>(desc.Width, UINT16_MAX);
    const uint32_t height = std::min<uint32_t>(desc.Height, UINT16_MAX);

    UINT move_bytes = 0;
    std::vector<DXGI_OUTDUPL_MOVE_RECT> move_rects;
    hr = duplication_->GetFrameMoveRects(0, nullptr, &move_bytes);
    if (hr == DXGI_ERROR_MORE_DATA && move_bytes > 0) {
        move_rects.resize(move_bytes / sizeof(DXGI_OUTDUPL_MOVE_RECT));
        hr = duplication_->GetFrameMoveRects(move_bytes, move_rects.data(), &move_bytes);
        if (FAILED(hr)) {
            move_rects.clear();
        }
    }

    UINT dirty_bytes = 0;
    std::vector<RECT> dirty_rects;
    hr = duplication_->GetFrameDirtyRects(0, nullptr, &dirty_bytes);
    if (hr == DXGI_ERROR_MORE_DATA && dirty_bytes > 0) {
        dirty_rects.resize(dirty_bytes / sizeof(RECT));
        hr = duplication_->GetFrameDirtyRects(dirty_bytes, dirty_rects.data(), &dirty_bytes);
        if (FAILED(hr)) {
            dirty_rects.clear();
        }
    }

    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.BindFlags = 0;
    staging_desc.MiscFlags = 0;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    hr = d3d_device_->CreateTexture2D(&staging_desc, nullptr, &staging);
    if (FAILED(hr)) {
        err = hrError("CreateTexture2D staging", hr);
        release_frame();
        return false;
    }
    d3d_context_->CopyResource(staging.Get(), desktop_texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = d3d_context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        err = hrError("Map staging", hr);
        release_frame();
        return false;
    }

    frame.width = static_cast<uint16_t>(width);
    frame.height = static_cast<uint16_t>(height);
    frame.stride = width * 4;
    frame.bgra.resize(static_cast<size_t>(frame.stride) * frame.height);
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(frame.bgra.data() + static_cast<size_t>(y) * frame.stride,
                    src + static_cast<size_t>(y) * mapped.RowPitch,
                    frame.stride);
    }
    d3d_context_->Unmap(staging.Get(), 0);
    release_frame();
    frame.dirty_rects.clear();
    if (!move_rects.empty()) {
        frame.dirty_rects = {Rect{0, 0, frame.width, frame.height}};
    } else {
        for (const RECT& dirty : dirty_rects) {
            const LONG left = std::max<LONG>(0, dirty.left);
            const LONG top = std::max<LONG>(0, dirty.top);
            const LONG right = std::min<LONG>(static_cast<LONG>(frame.width), dirty.right);
            const LONG bottom = std::min<LONG>(static_cast<LONG>(frame.height), dirty.bottom);
            if (right > left && bottom > top) {
                frame.dirty_rects.push_back(Rect{
                    static_cast<uint16_t>(left),
                    static_cast<uint16_t>(top),
                    static_cast<uint16_t>(right - left),
                    static_cast<uint16_t>(bottom - top),
                });
            }
        }
    }
    if (frame.dirty_rects.empty()) {
        frame.dirty_rects = {Rect{0, 0, frame.width, frame.height}};
    }
    last_frame_ = frame;
    return true;
}

bool WindowsScreenCapturer::captureWithGdi(ScreenFrame& frame, std::string& err) {
    flushDwmComposition();

    HDC screen = GetDC(nullptr);
    if (!screen) {
        err = "GetDC failed";
        return false;
    }

    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX) {
        ReleaseDC(nullptr, screen);
        err = "unsupported GDI desktop size";
        return false;
    }
    HDC memory = CreateCompatibleDC(screen);
    if (!memory) {
        ReleaseDC(nullptr, screen);
        err = "CreateCompatibleDC failed";
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        err = "CreateDIBSection failed";
        return false;
    }

    HGDIOBJ old = SelectObject(memory, bitmap);
    BOOL copied = BitBlt(memory, 0, 0, width, height, screen, 0, 0, SRCCOPY | CAPTUREBLT);
    SelectObject(memory, old);
    if (!copied) {
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        err = "BitBlt failed";
        return false;
    }

    const size_t byte_count = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    frame.width = static_cast<uint16_t>(width);
    frame.height = static_cast<uint16_t>(height);
    frame.stride = static_cast<uint32_t>(width) * 4;
    frame.bgra.assign(static_cast<uint8_t*>(pixels), static_cast<uint8_t*>(pixels) + byte_count);
    frame.dirty_rects = {Rect{0, 0, frame.width, frame.height}};
    last_frame_ = frame;

    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return true;
}

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
