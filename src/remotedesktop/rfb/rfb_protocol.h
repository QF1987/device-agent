#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "remotedesktop/input_injector.h"
#include "remotedesktop/screen_capturer.h"

namespace device_agent::remotedesktop::rfb {

// Clean-room RFB 3.8 implementation from RFC 6143. Do not copy GPL VNC code.
constexpr uint8_t kSecurityNone = 1;
constexpr int32_t kEncodingRaw = 0;

struct PixelFormat {
    uint8_t bits_per_pixel = 32;
    uint8_t depth = 24;
    uint8_t big_endian = 0;
    uint8_t true_color = 1;
    uint16_t red_max = 255;
    uint16_t green_max = 255;
    uint16_t blue_max = 255;
    uint8_t red_shift = 16;
    uint8_t green_shift = 8;
    uint8_t blue_shift = 0;
};

struct SetEncodings {
    std::vector<int32_t> encodings;
};

struct FramebufferUpdateRequest {
    bool incremental = false;
    Rect rect;
};

struct KeyEvent {
    bool down = false;
    uint32_t keysym = 0;
};

struct PointerEvent {
    uint8_t button_mask = 0;
    uint16_t x = 0;
    uint16_t y = 0;
};

struct ClientMessage {
    enum class Type {
        SetPixelFormat,
        SetEncodings,
        FramebufferUpdateRequest,
        KeyEvent,
        PointerEvent,
        ClientCutText,
    };

    Type type;
    PixelFormat pixel_format;
    SetEncodings set_encodings;
    FramebufferUpdateRequest framebuffer_update_request;
    KeyEvent key_event;
    PointerEvent pointer_event;
    std::string cut_text;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size);

    bool readU8(uint8_t& value);
    bool readU16(uint16_t& value);
    bool readU32(uint32_t& value);
    bool skip(size_t count);
    bool readBytes(size_t count, std::vector<uint8_t>& out);
    size_t remaining() const;

private:
    const uint8_t* data_;
    size_t size_;
    size_t offset_ = 0;
};

class RfbProtocol {
public:
    explicit RfbProtocol(std::string desktop_name = "DeviceOps VNC");

    const PixelFormat& pixelFormat() const { return pixel_format_; }
    void setPixelFormat(const PixelFormat& format) { pixel_format_ = format; }

    std::vector<uint8_t> protocolVersion() const;
    bool acceptClientVersion(const std::vector<uint8_t>& message, std::string& err) const;
    std::vector<uint8_t> securityTypes() const;
    bool acceptSecurityType(uint8_t security_type, std::string& err) const;
    std::vector<uint8_t> securityResultOk() const;
    std::vector<uint8_t> serverInit(uint16_t width, uint16_t height) const;

    bool parseClientMessage(const std::vector<uint8_t>& bytes, ClientMessage& message, std::string& err);
    std::vector<uint8_t> framebufferUpdate(const ScreenFrame& frame, const std::vector<Rect>& requested_rects) const;

private:
    uint32_t convertBgraPixel(const uint8_t* bgra) const;
    void appendPixel(std::vector<uint8_t>& out, uint32_t pixel) const;

    std::string desktop_name_;
    PixelFormat pixel_format_;
};

void appendU8(std::vector<uint8_t>& out, uint8_t value);
void appendU16(std::vector<uint8_t>& out, uint16_t value);
void appendU32(std::vector<uint8_t>& out, uint32_t value);
void appendS32(std::vector<uint8_t>& out, int32_t value);
void appendPixelFormat(std::vector<uint8_t>& out, const PixelFormat& format);

}  // namespace device_agent::remotedesktop::rfb
