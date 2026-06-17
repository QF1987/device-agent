#ifdef _WIN32

#include "remotedesktop/platform/windows/windows_screen_capturer.h"

namespace device_agent::remotedesktop::windows {

bool WindowsScreenCapturer::capture(ScreenFrame& frame, std::string& err) {
    // Win7-safe fallback. DXGI Desktop Duplication should sit above this branch
    // for Win8+ interactive sessions; session 0 still needs active-session launch.
    return captureWithGdi(frame, err);
}

bool WindowsScreenCapturer::captureWithGdi(ScreenFrame& frame, std::string& err) {
    HDC screen = GetDC(nullptr);
    if (!screen) {
        err = "GetDC failed";
        return false;
    }

    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
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

    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return true;
}

}  // namespace device_agent::remotedesktop::windows

#endif  // _WIN32
