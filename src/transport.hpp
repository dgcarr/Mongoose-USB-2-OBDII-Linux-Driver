#pragma once
#include "codec.hpp"
#include "mongoose/j2534.h"
#include <functional>
#include <memory>
#include <stdexcept>
#include <chrono>
namespace mongoose {
class Error : public std::runtime_error {
public:
    Error(int32_t status_code, const std::string &message) : std::runtime_error(message), code(status_code) {}
    int32_t code;
};
using Receiver = std::function<void(std::span<const uint8_t>)>;
using Failure = std::function<void(const std::string &)>;
using Trace = std::function<void(const std::string &, std::span<const uint8_t>)>;
class Transport {
public:
    virtual ~Transport() = default;
    virtual void start(Receiver receiver, Failure failure) = 0;
    virtual void send(std::span<const uint8_t> bytes, unsigned timeout_ms) = 0;
    // Stop must join callbacks before returning and be idempotent, even on error.
    virtual void stop() = 0;
};
struct DeviceInfo { std::string serial, location, error; };
std::vector<DeviceInfo> usb_devices();
std::unique_ptr<Transport> usb_transport(const std::string &serial, Trace trace = {});
}
