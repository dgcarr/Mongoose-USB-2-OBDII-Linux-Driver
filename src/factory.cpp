#include "transport.hpp"
namespace mongoose {
namespace {
#ifndef MONGOOSE_WITH_LIBUSB
[[noreturn]] void no_libusb() {
    throw Error(ERR_NOT_SUPPORTED, "this build has no libusb backend; rebuild with MONGOOSE_LIBUSB=ON");
}
#endif
}
std::vector<DeviceInfo> list_devices(Backend backend) {
    if (backend == Backend::Tty) return tty_devices();
#ifdef MONGOOSE_WITH_LIBUSB
    return usb_devices();
#else
    no_libusb();
#endif
}
std::unique_ptr<Transport> open_transport(const Selector &selector, Trace trace) {
    if (selector.backend == Backend::Tty) return tty_transport(selector, std::move(trace));
#ifdef MONGOOSE_WITH_LIBUSB
    return usb_transport(selector.serial, std::move(trace));
#else
    static_cast<void>(trace);
    no_libusb();
#endif
}
}
