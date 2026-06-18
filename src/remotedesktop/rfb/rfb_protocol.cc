#include "remotedesktop/rfb/rfb_protocol.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace device_agent::remotedesktop::rfb {

namespace {

uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint16_t scale_to_max(uint8_t value, uint16_t max) {
    if (max == 255) {
        return value;
    }
    return static_cast<uint16_t>((static_cast<uint32_t>(value) * max + 127) / 255);
}

Rect clamp_rect(const Rect& r, uint16_t width, uint16_t height) {
    Rect out = r;
    if (out.x >= width || out.y >= height) {
        out.width = 0;
        out.height = 0;
        return out;
    }
    out.width = std::min<uint16_t>(out.width, width - out.x);
    out.height = std::min<uint16_t>(out.height, height - out.y);
    return out;
}

std::vector<uint8_t> zlibStoredSyncFlush(const std::vector<uint8_t>& plain, bool include_header) {
    std::vector<uint8_t> out;
    if (include_header) {
        out.push_back(0x78);
        out.push_back(0x01);
    }
    size_t offset = 0;
    while (offset < plain.size()) {
        const size_t remaining = plain.size() - offset;
        const uint16_t len = static_cast<uint16_t>(std::min<size_t>(remaining, 65535));
        out.push_back(0x00);
        out.push_back(static_cast<uint8_t>(len & 0xff));
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
        const uint16_t nlen = static_cast<uint16_t>(~len);
        out.push_back(static_cast<uint8_t>(nlen & 0xff));
        out.push_back(static_cast<uint8_t>((nlen >> 8) & 0xff));
        out.insert(out.end(), plain.begin() + static_cast<std::ptrdiff_t>(offset),
                   plain.begin() + static_cast<std::ptrdiff_t>(offset + len));
        offset += len;
    }
    out.insert(out.end(), {0x00, 0x00, 0x00, 0xff, 0xff});
    return out;
}

}  // namespace

ByteReader::ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

bool ByteReader::readU8(uint8_t& value) {
    if (remaining() < 1) {
        return false;
    }
    value = data_[offset_++];
    return true;
}

bool ByteReader::readU16(uint16_t& value) {
    if (remaining() < 2) {
        return false;
    }
    value = read_be16(data_ + offset_);
    offset_ += 2;
    return true;
}

bool ByteReader::readU32(uint32_t& value) {
    if (remaining() < 4) {
        return false;
    }
    value = read_be32(data_ + offset_);
    offset_ += 4;
    return true;
}

bool ByteReader::skip(size_t count) {
    if (remaining() < count) {
        return false;
    }
    offset_ += count;
    return true;
}

bool ByteReader::readBytes(size_t count, std::vector<uint8_t>& out) {
    if (remaining() < count) {
        return false;
    }
    out.assign(data_ + offset_, data_ + offset_ + count);
    offset_ += count;
    return true;
}

size_t ByteReader::remaining() const {
    return size_ - offset_;
}

RfbProtocol::RfbProtocol(std::string desktop_name) : desktop_name_(std::move(desktop_name)) {}

void RfbProtocol::resetSessionState() {
    preferred_encoding_ = kEncodingRaw;
    zrle_stream_started_ = false;
}

std::vector<uint8_t> RfbProtocol::protocolVersion() const {
    const char version[] = "RFB 003.008\n";
    return std::vector<uint8_t>(version, version + 12);
}

bool RfbProtocol::acceptClientVersion(const std::vector<uint8_t>& message, std::string& err) const {
    if (message.size() != 12) {
        err = "RFB ProtocolVersion must be 12 bytes";
        return false;
    }
    if (std::memcmp(message.data(), "RFB 003.", 8) != 0 || message[11] != '\n') {
        err = "unsupported RFB ProtocolVersion prefix";
        return false;
    }
    const std::string minor(reinterpret_cast<const char*>(message.data() + 8), 3);
    if (minor != "003" && minor != "007" && minor != "008") {
        err = "unsupported RFB ProtocolVersion minor";
        return false;
    }
    return true;
}

std::vector<uint8_t> RfbProtocol::securityTypes() const {
    return {1, kSecurityNone};
}

bool RfbProtocol::acceptSecurityType(uint8_t security_type, std::string& err) const {
    if (security_type != kSecurityNone) {
        err = "only RFB security type None is supported";
        return false;
    }
    return true;
}

std::vector<uint8_t> RfbProtocol::securityResultOk() const {
    return {0, 0, 0, 0};
}

std::vector<uint8_t> RfbProtocol::serverInit(uint16_t width, uint16_t height) const {
    std::vector<uint8_t> out;
    appendU16(out, width);
    appendU16(out, height);
    appendPixelFormat(out, pixel_format_);
    appendU32(out, static_cast<uint32_t>(desktop_name_.size()));
    out.insert(out.end(), desktop_name_.begin(), desktop_name_.end());
    return out;
}

bool RfbProtocol::parseClientMessage(const std::vector<uint8_t>& bytes, ClientMessage& message, std::string& err) {
    if (bytes.empty()) {
        err = "empty RFB client message";
        return false;
    }
    ByteReader r(bytes.data(), bytes.size());
    uint8_t type = 0;
    if (!r.readU8(type)) {
        err = "missing RFB message type";
        return false;
    }

    switch (type) {
    case 0: {
        message.type = ClientMessage::Type::SetPixelFormat;
        if (!r.skip(3)) {
            err = "short SetPixelFormat padding";
            return false;
        }
        PixelFormat f;
        uint16_t u16 = 0;
        if (!r.readU8(f.bits_per_pixel) || !r.readU8(f.depth) || !r.readU8(f.big_endian) ||
            !r.readU8(f.true_color) || !r.readU16(f.red_max) || !r.readU16(f.green_max) ||
            !r.readU16(f.blue_max) || !r.readU8(f.red_shift) || !r.readU8(f.green_shift) ||
            !r.readU8(f.blue_shift) || !r.skip(3)) {
            err = "short SetPixelFormat body";
            return false;
        }
        (void)u16;
        if (f.bits_per_pixel != 8 && f.bits_per_pixel != 16 && f.bits_per_pixel != 32) {
            err = "unsupported bits-per-pixel";
            return false;
        }
        if (!f.true_color) {
            err = "color-map pixel formats are not supported";
            return false;
        }
        pixel_format_ = f;
        message.pixel_format = f;
        return true;
    }
    case 2: {
        message.type = ClientMessage::Type::SetEncodings;
        uint16_t count = 0;
        if (!r.skip(1) || !r.readU16(count)) {
            err = "short SetEncodings header";
            return false;
        }
        message.set_encodings.encodings.clear();
        for (uint16_t i = 0; i < count; ++i) {
            uint32_t value = 0;
            if (!r.readU32(value)) {
                err = "short SetEncodings list";
                return false;
            }
            message.set_encodings.encodings.push_back(static_cast<int32_t>(value));
        }
        preferred_encoding_ = kEncodingRaw;
        for (int32_t encoding : message.set_encodings.encodings) {
            if (encoding == kEncodingZrle) {
                preferred_encoding_ = kEncodingZrle;
                break;
            }
        }
        return true;
    }
    case 3: {
        message.type = ClientMessage::Type::FramebufferUpdateRequest;
        uint8_t incremental = 0;
        Rect rect;
        if (!r.readU8(incremental) || !r.readU16(rect.x) || !r.readU16(rect.y) ||
            !r.readU16(rect.width) || !r.readU16(rect.height)) {
            err = "short FramebufferUpdateRequest";
            return false;
        }
        message.framebuffer_update_request = {incremental != 0, rect};
        return true;
    }
    case 4: {
        message.type = ClientMessage::Type::KeyEvent;
        uint8_t down = 0;
        uint32_t keysym = 0;
        if (!r.readU8(down) || !r.skip(2) || !r.readU32(keysym)) {
            err = "short KeyEvent";
            return false;
        }
        message.key_event = {down != 0, keysym};
        return true;
    }
    case 5: {
        message.type = ClientMessage::Type::PointerEvent;
        PointerEvent ev;
        if (!r.readU8(ev.button_mask) || !r.readU16(ev.x) || !r.readU16(ev.y)) {
            err = "short PointerEvent";
            return false;
        }
        message.pointer_event = ev;
        return true;
    }
    case 6: {
        message.type = ClientMessage::Type::ClientCutText;
        uint32_t len = 0;
        if (!r.skip(3) || !r.readU32(len)) {
            err = "short ClientCutText header";
            return false;
        }
        if (len > r.remaining()) {
            err = "short ClientCutText payload";
            return false;
        }
        std::vector<uint8_t> text;
        if (!r.readBytes(len, text)) {
            err = "short ClientCutText payload";
            return false;
        }
        message.cut_text.assign(text.begin(), text.end());
        return true;
    }
    default:
        err = "unsupported RFB client message type";
        return false;
    }
}

std::vector<uint8_t> RfbProtocol::framebufferUpdate(const ScreenFrame& frame, const std::vector<Rect>& requested_rects) const {
    std::vector<Rect> rects;
    if (requested_rects.empty()) {
        rects.push_back(Rect{0, 0, frame.width, frame.height});
    } else {
        for (const auto& rect : requested_rects) {
            Rect clipped = clamp_rect(rect, frame.width, frame.height);
            if (clipped.width > 0 && clipped.height > 0) {
                rects.push_back(clipped);
            }
        }
    }
    if (rects.size() > std::numeric_limits<uint16_t>::max()) {
        rects.resize(std::numeric_limits<uint16_t>::max());
    }

    std::vector<uint8_t> out;
    appendU8(out, 0);  // FramebufferUpdate
    appendU8(out, 0);  // padding
    appendU16(out, static_cast<uint16_t>(rects.size()));

    for (const Rect& rect : rects) {
        appendU16(out, rect.x);
        appendU16(out, rect.y);
        appendU16(out, rect.width);
        appendU16(out, rect.height);
        if (preferred_encoding_ == kEncodingZrle) {
            appendS32(out, kEncodingZrle);
            std::vector<uint8_t> zrle = zrleRectPayload(frame, rect);
            appendU32(out, static_cast<uint32_t>(zrle.size()));
            out.insert(out.end(), zrle.begin(), zrle.end());
        } else {
            appendS32(out, kEncodingRaw);
            appendRawRect(out, frame, rect);
        }
    }
    return out;
}

uint32_t RfbProtocol::convertBgraPixel(const uint8_t* bgra) const {
    const uint16_t b = scale_to_max(bgra[0], pixel_format_.blue_max);
    const uint16_t g = scale_to_max(bgra[1], pixel_format_.green_max);
    const uint16_t r = scale_to_max(bgra[2], pixel_format_.red_max);
    return (static_cast<uint32_t>(r) << pixel_format_.red_shift) |
           (static_cast<uint32_t>(g) << pixel_format_.green_shift) |
           (static_cast<uint32_t>(b) << pixel_format_.blue_shift);
}

void RfbProtocol::appendPixel(std::vector<uint8_t>& out, uint32_t pixel) const {
    const uint8_t bytes = pixel_format_.bits_per_pixel / 8;
    if (pixel_format_.big_endian) {
        for (int i = bytes - 1; i >= 0; --i) {
            out.push_back(static_cast<uint8_t>((pixel >> (i * 8)) & 0xff));
        }
    } else {
        for (uint8_t i = 0; i < bytes; ++i) {
            out.push_back(static_cast<uint8_t>((pixel >> (i * 8)) & 0xff));
        }
    }
}

void RfbProtocol::appendCompressedPixel(std::vector<uint8_t>& out, uint32_t pixel) const {
    const size_t before = out.size();
    appendPixel(out, pixel);
    if (pixel_format_.bits_per_pixel == 32 && pixel_format_.true_color && pixel_format_.depth <= 24) {
        if (pixel_format_.big_endian) {
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(before));
        } else {
            out.pop_back();
        }
    }
}

void RfbProtocol::appendRawRect(std::vector<uint8_t>& out, const ScreenFrame& frame, const Rect& rect) const {
    const uint32_t stride = frame.stride == 0 ? static_cast<uint32_t>(frame.width) * 4 : frame.stride;
    for (uint16_t y = 0; y < rect.height; ++y) {
        size_t row = static_cast<size_t>(rect.y + y) * stride;
        for (uint16_t x = 0; x < rect.width; ++x) {
            size_t offset = row + static_cast<size_t>(rect.x + x) * 4;
            if (offset + 3 < frame.bgra.size()) {
                appendPixel(out, convertBgraPixel(frame.bgra.data() + offset));
            } else {
                appendPixel(out, 0);
            }
        }
    }
}

std::vector<uint8_t> RfbProtocol::zrleRectPayload(const ScreenFrame& frame, const Rect& rect) const {
    std::vector<uint8_t> tiles;
    const uint32_t stride = frame.stride == 0 ? static_cast<uint32_t>(frame.width) * 4 : frame.stride;
    auto pixel_at = [&](uint16_t x, uint16_t y) -> uint32_t {
        const size_t row = static_cast<size_t>(y) * stride;
        const size_t offset = row + static_cast<size_t>(x) * 4;
        if (offset + 3 < frame.bgra.size()) {
            return convertBgraPixel(frame.bgra.data() + offset);
        }
        return 0;
    };

    for (uint16_t tile_y = 0; tile_y < rect.height; tile_y = static_cast<uint16_t>(tile_y + 64)) {
        uint16_t tile_h = std::min<uint16_t>(64, rect.height - tile_y);
        for (uint16_t tile_x = 0; tile_x < rect.width; tile_x = static_cast<uint16_t>(tile_x + 64)) {
            uint16_t tile_w = std::min<uint16_t>(64, rect.width - tile_x);
            const uint16_t first_x = static_cast<uint16_t>(rect.x + tile_x);
            const uint16_t first_y = static_cast<uint16_t>(rect.y + tile_y);
            const uint32_t first_pixel = pixel_at(first_x, first_y);
            bool solid = true;
            for (uint16_t y = 0; solid && y < tile_h; ++y) {
                const uint16_t py = static_cast<uint16_t>(rect.y + tile_y + y);
                for (uint16_t x = 0; x < tile_w; ++x) {
                    const uint16_t px = static_cast<uint16_t>(rect.x + tile_x + x);
                    if (pixel_at(px, py) != first_pixel) {
                        solid = false;
                        break;
                    }
                }
            }
            if (solid) {
                tiles.push_back(1);  // Solid ZRLE tile.
                appendCompressedPixel(tiles, first_pixel);
                continue;
            }

            tiles.push_back(0);  // Raw ZRLE tile.
            for (uint16_t y = 0; y < tile_h; ++y) {
                for (uint16_t x = 0; x < tile_w; ++x) {
                    const uint16_t px = static_cast<uint16_t>(rect.x + tile_x + x);
                    const uint16_t py = static_cast<uint16_t>(rect.y + tile_y + y);
                    appendCompressedPixel(tiles, pixel_at(px, py));
                }
            }
        }
    }
    std::vector<uint8_t> out = zlibStoredSyncFlush(tiles, !zrle_stream_started_);
    zrle_stream_started_ = true;
    return out;
}

void appendU8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void appendU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void appendS32(std::vector<uint8_t>& out, int32_t value) {
    appendU32(out, static_cast<uint32_t>(value));
}

void appendPixelFormat(std::vector<uint8_t>& out, const PixelFormat& format) {
    appendU8(out, format.bits_per_pixel);
    appendU8(out, format.depth);
    appendU8(out, format.big_endian);
    appendU8(out, format.true_color);
    appendU16(out, format.red_max);
    appendU16(out, format.green_max);
    appendU16(out, format.blue_max);
    appendU8(out, format.red_shift);
    appendU8(out, format.green_shift);
    appendU8(out, format.blue_shift);
    appendU8(out, 0);
    appendU8(out, 0);
    appendU8(out, 0);
}

}  // namespace device_agent::remotedesktop::rfb
