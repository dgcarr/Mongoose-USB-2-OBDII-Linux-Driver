#include "transport.hpp"
namespace mongoose {
namespace {
constexpr char accepted[] =
    "Open name must be NULL, serial:S, tty:[serial:S|/dev/ttyACMn] or usb:[serial:S]";
bool consume(std::string_view &text, std::string_view prefix) {
    if (text.substr(0, prefix.size()) != prefix) return false;
    text.remove_prefix(prefix.size());
    return true;
}
// A remainder of "" means "the single connected adapter"; "serial:X" names one.
// Only the tty backend accepts a node path, since libusb has no such namespace.
void parse_remainder(std::string_view rest, Selector &selector, bool allow_path) {
    if (rest.empty()) return;
    if (allow_path && rest.front() == '/') {
        if (rest.find("..") != std::string_view::npos)
            throw Error(ERR_FAILED, "device node path must not contain '..'");
        selector.path.assign(rest);
        return;
    }
    if (!consume(rest, "serial:")) throw Error(ERR_FAILED, accepted);
    if (rest.empty()) throw Error(ERR_FAILED, "serial: needs a serial number");
    selector.serial.assign(rest);
}
}
Selector parse_selector(std::string_view name) {
    Selector selector;
    if (name.empty()) return selector;
    std::string_view rest = name;
    if (consume(rest, "tty:")) { parse_remainder(rest, selector, true); return selector; }
    if (consume(rest, "usb:")) {
        selector.backend = Backend::Usb;
        parse_remainder(rest, selector, false);
        return selector;
    }
    // Bare serial: keeps working and now means "that adapter over the default backend".
    if (name.substr(0, 7) == "serial:") { parse_remainder(name, selector, false); return selector; }
    throw Error(ERR_FAILED, accepted);
}
}
