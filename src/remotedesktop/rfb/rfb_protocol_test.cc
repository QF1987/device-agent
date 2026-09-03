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

uint32_t read_be32_at(const std::vector<uint8_t>& data, size_t offset) {
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) |
           static_cast<uint32_t>(data[offset + 3]);
}

std::vector<uint8_t> storedZlibPlain(const std::vector<uint8_t>& data, size_t offset, size_t size) {
    std::vector<uint8_t> plain;
    size_t pos = offset;
    const size_t end = offset + size;
    if (pos + 2 <= end && data[pos] == 0x78 && data[pos + 1] == 0x01) {
        pos += 2;
    }
    while (pos + 5 <= end) {
        if (pos + 5 == end && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
            data[pos + 2] == 0x00 && data[pos + 3] == 0xff && data[pos + 4] == 0xff) {
            break;
        }
        assert(data[pos] == 0x00);
        const uint16_t len = static_cast<uint16_t>(data[pos + 1] | (static_cast<uint16_t>(data[pos + 2]) << 8));
        const uint16_t nlen = static_cast<uint16_t>(data[pos + 3] | (static_cast<uint16_t>(data[pos + 4]) << 8));
        assert(static_cast<uint16_t>(~len) == nlen);
        pos += 5;
        assert(pos + len <= end);
        plain.insert(plain.end(), data.begin() + static_cast<std::ptrdiff_t>(pos),
                     data.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
    }
    return plain;
}

ScreenFrame solidFrame(uint16_t width, uint16_t height, uint8_t value = 0) {
    ScreenFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = static_cast<uint32_t>(width) * 4;
    frame.bgra.assign(static_cast<size_t>(frame.stride) * height, value);
    return frame;
}

void setPixel(ScreenFrame& frame, uint16_t x, uint16_t y, uint8_t value) {
    const size_t offset = static_cast<size_t>(y) * frame.stride + static_cast<size_t>(x) * 4;
    assert(offset + 3 < frame.bgra.size());
    frame.bgra[offset] = value;
    frame.bgra[offset + 1] = value;
    frame.bgra[offset + 2] = value;
    frame.bgra[offset + 3] = 0xff;
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
    uint32_t length = read_be32_at(out, 16);
    assert(length == out.size() - 20);
    assert(out[20] == 0x78);
    auto plain = storedZlibPlain(out, 20, length);
    assert(plain.size() == 7);
    assert(plain[0] == 0);
}

void testZrleSolidTile() {
    RfbProtocol rfb;
    ClientMessage msg{ClientMessage::Type::SetEncodings};
    std::string err;
    std::vector<uint8_t> zrle_enc{2, 0, 0, 1, 0, 0, 0, 16};
    assert(rfb.parseClientMessage(zrle_enc, msg, err));

    ScreenFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.stride = 8;
    frame.bgra = {
        0x03, 0x02, 0x01, 0xff,
        0x03, 0x02, 0x01, 0xff,
        0x03, 0x02, 0x01, 0xff,
        0x03, 0x02, 0x01, 0xff,
    };
    auto out = rfb.framebufferUpdate(frame, {Rect{0, 0, 2, 2}});
    const uint32_t length = read_be32_at(out, 16);
    assert(length == out.size() - 20);
    auto plain = storedZlibPlain(out, 20, length);
    assert(plain.size() == 4);
    assert(plain[0] == 1);
    // Default little-endian 32bpp/depth24 CPIXEL omits the unused alpha byte.
    assert(plain[1] == 0x03 && plain[2] == 0x02 && plain[3] == 0x01);
}

void testTileDirtyRegions() {
    ScreenFrame previous = solidFrame(64, 64);
    ScreenFrame current = previous;
    auto rects = device_agent::remotedesktop::computeTileDirtyRects(previous, current, 16);
    assert(rects.empty());

    setPixel(current, 20, 35, 0x40);
    setPixel(current, 20, 50, 0x40);
    rects = device_agent::remotedesktop::computeTileDirtyRects(previous, current, 16);
    assert(rects.size() == 1);
    assert(rects[0].x == 16 && rects[0].y == 32);
    assert(rects[0].width == 16 && rects[0].height == 32);
    RfbProtocol raw;
    const auto small_update = raw.framebufferUpdate(current, rects);
    const auto full_update = raw.framebufferUpdate(
        current, {Rect{0, 0, current.width, current.height}});
    assert(small_update.size() < full_update.size() / 4);

    ScreenFrame first;
    rects = device_agent::remotedesktop::computeTileDirtyRects(first, current, 16);
    assert(rects.size() == 1);
    assert(rects[0].x == 0 && rects[0].y == 0);
    assert(rects[0].width == 64 && rects[0].height == 64);

    ScreenFrame mostly_changed = solidFrame(64, 64, 0x7f);
    rects = device_agent::remotedesktop::computeTileDirtyRects(previous, mostly_changed, 16);
    assert(rects.size() == 1);
    assert(rects[0].width == 64 && rects[0].height == 64);

    ScreenFrame fragmented = previous;
    setPixel(fragmented, 1, 1, 0x20);
    setPixel(fragmented, 33, 1, 0x20);
    setPixel(fragmented, 1, 33, 0x20);
    rects = device_agent::remotedesktop::computeTileDirtyRects(previous, fragmented, 16, 2);
    assert(rects.size() == 1);
    assert(rects[0].width == 64 && rects[0].height == 64);
}

void testEmptyIncrementalFramebufferUpdate() {
    RfbProtocol rfb;
    ScreenFrame frame = solidFrame(64, 64);
    const auto empty = rfb.framebufferUpdate(frame, {});
    assert(empty == std::vector<uint8_t>({0, 0, 0, 0}));

    const auto full = rfb.framebufferUpdate(frame, {Rect{0, 0, frame.width, frame.height}});
    assert(full.size() == 4 + 12 + static_cast<size_t>(frame.width) * frame.height * 4);
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

class StaticCapturer : public device_agent::remotedesktop::IScreenCapturer {
public:
    bool capture(ScreenFrame& frame, std::string&) override {
        frame = solidFrame(2, 1);
        if (calls++ == 0) {
            frame.dirty_rects = {Rect{0, 0, 2, 1}};
        }
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

void testRfbServerSendsEmptyIncrementalUpdate() {
    std::vector<uint8_t> input = bytes("RFB 003.008\n");
    input.push_back(1);  // Security type: None.
    input.push_back(1);  // Shared flag.
    std::vector<uint8_t> fur{3, 1, 0, 0, 0, 0, 0, 2, 0, 1};
    input.insert(input.end(), fur.begin(), fur.end());

    StaticCapturer capturer;
    NoopInjector injector;
    RfbServer server(capturer, injector, "Static Test Desktop");
    MemoryTransport transport(std::move(input));
    std::string err;
    assert(!server.serveClient(transport, err));
    assert(err == "synthetic EOF");
    assert(capturer.calls == 2);
    assert(transport.output.size() >= 4);
    const size_t end = transport.output.size();
    assert(transport.output[end - 4] == 0 && transport.output[end - 3] == 0 &&
           transport.output[end - 2] == 0 && transport.output[end - 1] == 0);
}

}  // namespace

int main() {
    testHandshake();
    testClientMessages();
    testRawFramebufferUpdate();
    testZrleFramebufferUpdate();
    testZrleSolidTile();
    testTileDirtyRegions();
    testEmptyIncrementalFramebufferUpdate();
    testTunnelFrames();
    testRfbServerRetriesTransientCaptureFailure();
    testRfbServerSendsEmptyIncrementalUpdate();
    std::cout << "rfb_protocol_test PASS\n";
    return 0;
}
