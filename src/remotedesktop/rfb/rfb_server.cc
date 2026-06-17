#include "remotedesktop/rfb/rfb_server.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace device_agent::remotedesktop::rfb {

namespace {

size_t clientMessageSize(uint8_t type, IRfbTransport& transport, std::vector<uint8_t>& bytes, std::string& err) {
    switch (type) {
    case 0: return 20;
    case 2: {
        uint8_t header[3]{};
        if (!transport.readExact(header, sizeof(header), err)) {
            return 0;
        }
        bytes.insert(bytes.end(), header, header + sizeof(header));
        uint16_t count = static_cast<uint16_t>((header[1] << 8) | header[2]);
        return 4 + static_cast<size_t>(count) * 4;
    }
    case 3: return 10;
    case 4: return 8;
    case 5: return 6;
    case 6: {
        uint8_t header[7]{};
        if (!transport.readExact(header, sizeof(header), err)) {
            return 0;
        }
        bytes.insert(bytes.end(), header, header + sizeof(header));
        uint32_t len = (static_cast<uint32_t>(header[3]) << 24) |
                       (static_cast<uint32_t>(header[4]) << 16) |
                       (static_cast<uint32_t>(header[5]) << 8) |
                       static_cast<uint32_t>(header[6]);
        return 8 + len;
    }
    default:
        err = "unsupported RFB client message type";
        return 0;
    }
}

}  // namespace

RfbServer::RfbServer(IScreenCapturer& capturer, IInputInjector& injector, std::string desktop_name)
    : capturer_(capturer), injector_(injector), protocol_(std::move(desktop_name)) {}

bool RfbServer::serveClient(IRfbTransport& transport, std::string& err) {
    protocol_.resetSessionState();
    if (!writeVector(transport, protocol_.protocolVersion(), err)) {
        return false;
    }
    std::vector<uint8_t> client_version(12);
    if (!transport.readExact(client_version.data(), client_version.size(), err)) {
        return false;
    }
    if (!protocol_.acceptClientVersion(client_version, err)) {
        return false;
    }
    if (!writeVector(transport, protocol_.securityTypes(), err)) {
        return false;
    }
    uint8_t security_type = 0;
    if (!transport.readExact(&security_type, 1, err)) {
        return false;
    }
    if (!protocol_.acceptSecurityType(security_type, err)) {
        return false;
    }
    if (!writeVector(transport, protocol_.securityResultOk(), err)) {
        return false;
    }

    uint8_t shared_flag = 0;
    if (!transport.readExact(&shared_flag, 1, err)) {
        return false;
    }
    (void)shared_flag;

    ScreenFrame initial;
    if (!captureWithRetry(initial, 30, 500, err)) {
        return false;
    }
    if (!writeVector(transport, protocol_.serverInit(initial.width, initial.height), err)) {
        return false;
    }

    for (;;) {
        ClientMessage message{ClientMessage::Type::SetEncodings};
        if (!readClientMessage(transport, message, err)) {
            return false;
        }
        switch (message.type) {
        case ClientMessage::Type::FramebufferUpdateRequest:
            if (!handleFramebufferRequest(transport, message.framebuffer_update_request, err)) {
                return false;
            }
            break;
        case ClientMessage::Type::KeyEvent:
            if (!injector_.keyEvent(message.key_event.keysym, message.key_event.down, err)) {
                return false;
            }
            break;
        case ClientMessage::Type::PointerEvent:
            if (!injector_.pointerEvent(message.pointer_event.button_mask, message.pointer_event.x, message.pointer_event.y, err)) {
                return false;
            }
            break;
        case ClientMessage::Type::SetPixelFormat:
        case ClientMessage::Type::SetEncodings:
        case ClientMessage::Type::ClientCutText:
            break;
        }
    }
}

bool RfbServer::writeVector(IRfbTransport& transport, const std::vector<uint8_t>& bytes, std::string& err) {
    return transport.writeAll(bytes.data(), bytes.size(), err);
}

bool RfbServer::readClientMessage(IRfbTransport& transport, ClientMessage& message, std::string& err) {
    uint8_t type = 0;
    if (!transport.readExact(&type, 1, err)) {
        return false;
    }
    std::vector<uint8_t> bytes{type};
    size_t total = clientMessageSize(type, transport, bytes, err);
    if (total == 0) {
        return false;
    }
    if (bytes.size() < total) {
        size_t rest = total - bytes.size();
        size_t old = bytes.size();
        bytes.resize(total);
        if (!transport.readExact(bytes.data() + old, rest, err)) {
            return false;
        }
    }
    return protocol_.parseClientMessage(bytes, message, err);
}

bool RfbServer::captureWithRetry(ScreenFrame& frame, int attempts, int delay_ms, std::string& err) {
    std::string last_err;
    const int tries = std::max(1, attempts);
    for (int i = 0; i < tries; ++i) {
        if (capturer_.capture(frame, last_err)) {
            err.clear();
            return true;
        }
        if (i + 1 < tries) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    err = last_err.empty() ? "screen capture failed" : last_err;
    return false;
}

bool RfbServer::handleFramebufferRequest(IRfbTransport& transport, const FramebufferUpdateRequest& request, std::string& err) {
    ScreenFrame frame;
    if (!captureWithRetry(frame, 20, 250, err)) {
        return false;
    }

    std::vector<Rect> rects;
    if (request.incremental && !frame.dirty_rects.empty()) {
        rects = frame.dirty_rects;
    } else {
        rects = {request.rect};
    }
    return writeVector(transport, protocol_.framebufferUpdate(frame, rects), err);
}

}  // namespace device_agent::remotedesktop::rfb
