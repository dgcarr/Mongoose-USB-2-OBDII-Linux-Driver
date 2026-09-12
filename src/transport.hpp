#pragma once
#include "codec.hpp"
#include "mongoose/j2534.h"
#include <functional>
#include <memory>
#include <stdexcept>
#include <string_view>
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
    // timeout_ms is always non-zero; implementations may reject zero. The backends read
    // it oppositely -- libusb treats zero as "wait forever", poll() as "return at once" --
    // so Session rounds sub-millisecond budgets up rather than letting either see zero.
    virtual void send(std::span<const uint8_t> bytes, unsigned timeout_ms) = 0;
    // Stop must join callbacks before returning and be idempotent, even on error.
    virtual void stop() = 0;
};
struct DeviceInfo { std::string serial, location, error; };
// cdc_acm is the default: an ordinary /dev/ttyACM* client needing no libusb, no
// interface detaching and no root. There is no Auto member on purpose -- falling back
// to libusb would silently detach cdc_acm and make failures irreproducible.
enum class Backend { Tty, Usb };
struct Selector {
    Backend backend = Backend::Tty;
    std::string serial;  // empty: require exactly one matching adapter
    std::string path;    // tty only; empty: resolve the node from sysfs
};
// Pure string parsing, built into mongoose_core so the API tests exercise the real one.
// Throws Error(ERR_FAILED, ...) on an unrecognised name.
Selector parse_selector(std::string_view name);
std::vector<DeviceInfo> tty_devices();
std::unique_ptr<Transport> tty_transport(const Selector &selector, Trace trace = {});
// libusb backend, built only when the MONGOOSE_LIBUSB option finds the library.
std::vector<DeviceInfo> usb_devices();
std::unique_ptr<Transport> usb_transport(const std::string &serial, Trace trace = {});
// Backend dispatch; see factory.cpp.
std::unique_ptr<Transport> open_transport(const Selector &selector, Trace trace = {});
std::vector<DeviceInfo> list_devices(Backend backend);
}
