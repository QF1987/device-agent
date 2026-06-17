#pragma once

#include "remotedesktop/input_injector.h"
#include "remotedesktop/rfb/rfb_protocol.h"
#include "remotedesktop/screen_capturer.h"

#include <memory>

namespace device_agent::remotedesktop::rfb {

class IRfbTransport {
public:
    virtual ~IRfbTransport() = default;

    virtual bool readExact(uint8_t* data, size_t size, std::string& err) = 0;
    virtual bool writeAll(const uint8_t* data, size_t size, std::string& err) = 0;
};

class RfbServer {
public:
    RfbServer(IScreenCapturer& capturer, IInputInjector& injector, std::string desktop_name);

    bool serveClient(IRfbTransport& transport, std::string& err);

private:
    bool writeVector(IRfbTransport& transport, const std::vector<uint8_t>& bytes, std::string& err);
    bool readClientMessage(IRfbTransport& transport, ClientMessage& message, std::string& err);
    bool handleFramebufferRequest(IRfbTransport& transport, const FramebufferUpdateRequest& request, std::string& err);

    IScreenCapturer& capturer_;
    IInputInjector& injector_;
    RfbProtocol protocol_;
};

}  // namespace device_agent::remotedesktop::rfb
