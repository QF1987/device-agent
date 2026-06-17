#include "remotedesktop/rfb/rfb_protocol.h"
#include "remotedesktop/rfb/rfb_server.h"
#include "remotedesktop/tunnel/tunnel_protocol.h"

#include <cassert>
#include <cstring>
#include <iostream>

using device_agent::remotedesktop::Rect;
using device_agent::remotedesktop::ScreenFrame;
using device_agent::remotedesktop::rfb::ClientMessage;
using device_agent::remotedesktop::rfb::IRfbTransport;
using device_agent::remotedesktop::rfb::PixelFormat;
using device_agent::remotedesktop::rfb::RfbProtocol;
using device_agent::remotedesktop::rfb::RfbServer;

namespace {

std::vector<uint8_t> bytes(const char* s) {
    return std::vector<uint8_t>(s, s + std::strlen(s));
}

void testHandshake() {
    RfbProtocol rfb("Unit Test Desktop");
    std::string err;
    assert(rfb.protocolVersion() == bytes("RFB 003.008\n"));
    assert(rfb.acceptClientVersion(bytes("RFB 003.008\n"), err));
    assert(rfb.acceptClientVersion(bytes("RFB 003.003\n"), err));
    assert(!rfb.acceptClientVersion(bytes("RFB 004.000\n"), err));
    assert(rfb.securityTypes() == std::vector<uint8_t>({1, 1}));
    assert(rfb.acceptSecurityType(1, err));
    assert(!rfb.acceptSecurityType(2, err));
    assert(rfb.securityResultOk() == std::vector<uint8_t>({0, 0, 0, 0}));

    auto init = rfb.serverInit(640, 480);
    assert(init[0] == 0x02 && init[1] == 0x80);
    assert(init[2] == 0x01 && init[3] == 0xe0);
    assert(init[4] == 32);
    assert(init[5] == 24);
}

void testClientMessages() {
    RfbProtocol rfb;
    std::string err;
    ClientMessage msg{ClientMessage::Type::SetEncodings};

    std::vector<uint8_t> set_pf{
        0, 0, 0, 0,
        16, 16, 0, 1,
        0x00, 0x1f, 0x00, 0x3f, 0x00, 0x1f,
        11, 5, 0,
        0, 0, 0,
    };
    assert(rfb.parseClientMessage(set_pf, msg, err));
    assert(msg.type == ClientMessage::Type::SetPixelFormat);
    assert(rfb.pixelFormat().bits_per_pixel == 16);
    assert(rfb.pixelFormat().red_shift == 11);

    std::vector<uint8_t> enc{2, 0, 0, 2, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x21};
    assert(rfb.parseClientMessage(enc, msg, err));
    assert(msg.type == ClientMessage::Type::SetEncodings);
    assert(msg.set_encodings.encodings.size() == 2);
    assert(msg.set_encodings.encodings[0] == 0);
    assert(msg.set_encodings.encodings[1] == -223);
    assert(rfb.preferredEncoding() == device_agent::remotedesktop::rfb::kEncodingRaw);

    std::vector<uint8_t> zrle_enc{2, 0, 0, 2, 0, 0, 0, 16, 0, 0, 0, 0};
    assert(rfb.parseClientMessage(zrle_enc, msg, err));
    assert(msg.type == ClientMessage::Type::SetEncodings);
    assert(rfb.preferredEncoding() == device_agent::remotedesktop::rfb::kEncodingZrle);

    std::vector<uint8_t> fur{3, 1, 0, 10, 0, 20, 0, 30, 0, 40};
    assert(rfb.parseClientMessage(fur, msg, err));
    assert(msg.type == ClientMessage::Type::FramebufferUpdateRequest);
    assert(msg.framebuffer_update_request.incremental);
    assert(msg.framebuffer_update_request.rect.x == 10);
    assert(msg.framebuffer_update_request.rect.height == 40);

    std::vector<uint8_t> key{4, 1, 0, 0, 0, 0, 0xff, 0x0d};
    assert(rfb.parseClientMessage(key, msg, err));
    assert(msg.type == ClientMessage::Type::KeyEvent);
    assert(msg.key_event.down);
    assert(msg.key_event.keysym == 0xff0d);

    std::vector<uint8_t> ptr{5, 1, 0, 100, 0, 120};
    assert(rfb.parseClientMessage(ptr, msg, err));
    assert(msg.type == ClientMessage::Type::PointerEvent);
    assert(msg.pointer_event.button_mask == 1);
    assert(msg.pointer_event.x == 100);

    std::vector<uint8_t> cut{6, 0, 0, 0, 0, 0, 0, 2, 'o', 'k'};
    assert(rfb.parseClientMessage(cut, msg, err));
    assert(msg.type == ClientMessage::Type::ClientCutText);
    assert(msg.cut_text == "ok");
}

void testRawFramebufferUpdate() {
    RfbProtocol rfb;
    ScreenFrame frame;
    frame.width = 2;
    frame.height = 1;
    frame.stride = 8;
    frame.bgra = {
        0x03, 0x02, 0x01, 0xff,
        0x30, 0x20, 0x10, 0xff,
    };
    auto out = rfb.framebufferUpdate(frame, {Rect{0, 0, 2, 1}});
    assert(out.size() == 4 + 12 + 8);
    assert(out[0] == 0);
    assert(out[3] == 1);
    assert(out[12] == 0 && out[13] == 0 && out[14] == 0 && out[15] == 0);
    // Default little-endian true-color puts RGB value 0x010203 on the wire as 03 02 01 00.
    assert(out[16] == 0x03 && out[17] == 0x02 && out[18] == 0x01 && out[19] == 0x00);

    PixelFormat rgb565;
    rgb565.bits_per_pixel = 16;
    rgb565.depth = 16;
    rgb565.red_max = 31;
    rgb565.green_max = 63;
    rgb565.blue_max = 31;
    rgb565.red_shift = 11;
    rgb565.green_shift = 5;
    rgb565.blue_shift = 0;
    rfb.setPixelFormat(rgb565);
    auto rgb565_out = rfb.framebufferUpdate(frame, {Rect{0, 0, 1, 1}});
    assert(rgb565_out.size() == 4 + 12 + 2);
}

void testZrleFramebufferUpdate() {
    RfbProtocol rfb;
    ClientMessage msg{ClientMessage::Type::SetEncodings};
    std::string err;
    std::vector<uint8_t> zrle_enc{2, 0, 0, 1, 0, 0, 0, 16};
    assert(rfb.parseClientMessage(zrle_enc, msg, err));
    assert(rfb.preferredEncoding() == device_agent::remotedesktop::rfb::kEncodingZrle);

    ScreenFrame frame;
    frame.width = 2;
    frame.height = 1;
    frame.stride = 8;
    frame.bgra = {
        0x03, 0x02, 0x01, 0xff,
        0x30, 0x20, 0x10, 0xff,
    };
    auto out = rfb.framebufferUpdate(frame, {Rect{0, 0, 2, 1}});
    assert(out.size() > 20);
    assert(out[0] == 0);
    assert(out[3] == 1);
    assert(out[12] == 0 && out[13] == 0 && out[14] == 0 && out[15] == 16);
    uint32_t length = (static_cast<uint32_t>(out[16]) << 24) |
                      (static_cast<uint32_t>(out[17]) << 16) |
                      (static_cast<uint32_t>(out[18]) << 8) |
                      static_cast<uint32_t>(out[19]);
    assert(length == out.size() - 20);
    assert(out[20] == 0x78);
}

void testTunnelFrames() {
    using namespace device_agent::remotedesktop::tunnel;
    assert(helloFrame("dev-1", "tok", 1366, 768) == "HELLO\tdev-1\ttok\t1366\t768\n");
    assert(helloFrame("dev-1", "tok", 0, 0) == "HELLO\tdev-1\ttok\n");
    assert(dataFrame("stream", "dev-1", "tok") == "DATA\tstream\tdev-1\ttok\n");
    std::vector<std::string> fields;
    std::string err;
    assert(parseFrame("BADGE\ton\r\n", fields, err));
    assert(fields.size() == 2 && fields[0] == "BADGE" && fields[1] == "on");
}

class RetryCapturer : public device_agent::remotedesktop::IScreenCapturer {
public:
    bool capture(ScreenFrame& frame, std::string& err) override {
        ++calls;
        if (calls == 1 || calls == 3) {
            err = "synthetic transient capture failure";
            return false;
        }
        frame.width = 2;
        frame.height = 1;
        frame.stride = 8;
        frame.bgra = {0x03, 0x02, 0x01, 0xff, 0x30, 0x20, 0x10, 0xff};
        frame.dirty_rects = {Rect{0, 0, 2, 1}};
        return true;
    }

    int calls = 0;
};

class NoopInjector : public device_agent::remotedesktop::IInputInjector {
public:
    bool keyEvent(uint32_t, bool, std::string&) override { return true; }
    bool pointerEvent(uint8_t, uint16_t, uint16_t, std::string&) override { return true; }
};

class MemoryTransport : public IRfbTransport {
public:
    explicit MemoryTransport(std::vector<uint8_t> input) : input_(std::move(input)) {}

    bool readExact(uint8_t* data, size_t size, std::string& err) override {
        if (pos_ + size > input_.size()) {
            err = "synthetic EOF";
            return false;
        }
        std::memcpy(data, input_.data() + pos_, size);
        pos_ += size;
        return true;
    }

    bool writeAll(const uint8_t* data, size_t size, std::string&) override {
        output.insert(output.end(), data, data + size);
        return true;
    }

    std::vector<uint8_t> output;

private:
    std::vector<uint8_t> input_;
    size_t pos_ = 0;
};

void testRfbServerRetriesTransientCaptureFailure() {
    std::vector<uint8_t> input = bytes("RFB 003.008\n");
    input.push_back(1);  // Security type: None.
    input.push_back(1);  // Shared flag.
    std::vector<uint8_t> fur{3, 0, 0, 0, 0, 0, 0, 2, 0, 1};
    input.insert(input.end(), fur.begin(), fur.end());

    RetryCapturer capturer;
    NoopInjector injector;
    RfbServer server(capturer, injector, "Retry Test Desktop");
    MemoryTransport transport(std::move(input));
    std::string err;
    assert(!server.serveClient(transport, err));
    assert(err == "synthetic EOF");
    assert(capturer.calls == 4);
    assert(transport.output.size() >= bytes("RFB 003.008\n").size() + 2 + 4 + 24 + 4 + 12 + 8);
}

}  // namespace

int main() {
    testHandshake();
    testClientMessages();
    testRawFramebufferUpdate();
    testZrleFramebufferUpdate();
    testTunnelFrames();
    testRfbServerRetriesTransientCaptureFailure();
    std::cout << "rfb_protocol_test PASS\n";
    return 0;
}
