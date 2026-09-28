#pragma once
#include <functional>
#include <string>

// Pure transport interface (no provider logic)
class WsTransport {
public:
    using MessageCb = std::function<void(std::string)>; // own the data to avoid dangling views
    using StatusCb  = std::function<void(bool)>;
    using ErrorCb   = std::function<void(std::string)>;

    // Non-owning; implementation will decide how to manage io resources
    WsTransport() = default;
    virtual ~WsTransport() = default;

    virtual void connect(std::string host, std::string port, std::string target) = 0;
    virtual void close() = 0;
    virtual void send(std::string msg) = 0; // serialized by implementation

    virtual void onMessage(MessageCb) = 0;
    virtual void onStatus(StatusCb) = 0;
    virtual void onError(ErrorCb) = 0;
};


