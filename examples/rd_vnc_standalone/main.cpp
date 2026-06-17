#include "remotedesktop/input_injector.h"
#include "remotedesktop/rfb/rfb_server.h"
#include "remotedesktop/screen_capturer.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "remotedesktop/platform/windows/windows_input_injector.h"
#include "remotedesktop/platform/windows/windows_screen_capturer.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {

class TestPatternCapturer : public device_agent::remotedesktop::IScreenCapturer {
public:
    bool capture(device_agent::remotedesktop::ScreenFrame& frame, std::string&) override {
        frame.width = 320;
        frame.height = 200;
        frame.stride = frame.width * 4;
        frame.bgra.resize(static_cast<size_t>(frame.stride) * frame.height);
        tick_++;
        for (uint16_t y = 0; y < frame.height; ++y) {
            for (uint16_t x = 0; x < frame.width; ++x) {
                size_t offset = static_cast<size_t>(y) * frame.stride + static_cast<size_t>(x) * 4;
                frame.bgra[offset + 0] = static_cast<uint8_t>((x + tick_) & 0xff);
                frame.bgra[offset + 1] = static_cast<uint8_t>((y + tick_) & 0xff);
                frame.bgra[offset + 2] = static_cast<uint8_t>((x + y) & 0xff);
                frame.bgra[offset + 3] = 0xff;
            }
        }
        frame.dirty_rects = {device_agent::remotedesktop::Rect{0, 0, frame.width, frame.height}};
        return true;
    }

private:
    uint8_t tick_ = 0;
};

class NopInputInjector : public device_agent::remotedesktop::IInputInjector {
public:
    bool keyEvent(uint32_t, bool, std::string&) override { return true; }
    bool pointerEvent(uint8_t, uint16_t, uint16_t, std::string&) override { return true; }
};

#ifdef _WIN32
class SocketTransport : public device_agent::remotedesktop::rfb::IRfbTransport {
public:
    explicit SocketTransport(SOCKET socket) : socket_(socket) {}
    ~SocketTransport() override { closesocket(socket_); }

    bool readExact(uint8_t* data, size_t size, std::string& err) override {
        size_t got = 0;
        while (got < size) {
            int n = recv(socket_, reinterpret_cast<char*>(data + got), static_cast<int>(size - got), 0);
            if (n <= 0) {
                err = "recv failed";
                return false;
            }
            got += static_cast<size_t>(n);
        }
        return true;
    }

    bool writeAll(const uint8_t* data, size_t size, std::string& err) override {
        size_t sent = 0;
        while (sent < size) {
            int n = send(socket_, reinterpret_cast<const char*>(data + sent), static_cast<int>(size - sent), 0);
            if (n <= 0) {
                err = "send failed";
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

private:
    SOCKET socket_;
};
#endif

}  // namespace

int main(int argc, char** argv) {
    int port = 5901;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 4) != 0) {
        std::cerr << "listen failed\n";
        closesocket(listener);
        WSACleanup();
        return 1;
    }
    std::cout << "rd_vnc_standalone listening on :" << port << "\n";
    for (;;) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            break;
        }
        std::thread([client]() {
            device_agent::remotedesktop::windows::WindowsScreenCapturer capturer;
            device_agent::remotedesktop::windows::WindowsInputInjector injector;
            device_agent::remotedesktop::rfb::RfbServer server(capturer, injector, "DeviceOps Windows VNC");
            SocketTransport transport(client);
            std::string err;
            if (!server.serveClient(transport, err)) {
                std::cerr << "RFB session ended: " << err << "\n";
            }
        }).detach();
    }
    closesocket(listener);
    WSACleanup();
    return 0;
#else
    (void)port;
    TestPatternCapturer capturer;
    NopInputInjector injector;
    device_agent::remotedesktop::rfb::RfbServer server(capturer, injector, "DeviceOps Test Pattern");
    std::cout << "rd_vnc_standalone builds on this platform; TCP listener is Windows-only in Phase 1.\n";
    (void)server;
    return 0;
#endif
}
