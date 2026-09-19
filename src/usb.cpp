#include "transport.hpp"
#include <libusb.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
namespace mongoose {
namespace {
int32_t api_error(int code) {
    if (code == LIBUSB_ERROR_NO_DEVICE) return ERR_DEVICE_NOT_CONNECTED;
    if (code == LIBUSB_ERROR_BUSY) return ERR_DEVICE_IN_USE;
    if (code == LIBUSB_ERROR_TIMEOUT) return ERR_TIMEOUT;
    return ERR_FAILED;
}
void check(int code, const std::string &action) {
    if (code < 0) throw Error(api_error(code), action + ": " + libusb_error_name(code));
}
struct Context {
    libusb_context *value = nullptr;
    Context() { check(libusb_init(&value), "libusb initialization"); }
    ~Context() { libusb_exit(value); }
};
struct DeviceList {
    libusb_device **items = nullptr;
    ssize_t count;
    explicit DeviceList(libusb_context *ctx) : count(libusb_get_device_list(ctx, &items)) {
        if (count < 0) check(static_cast<int>(count), "enumerating USB devices");
    }
    ~DeviceList() { libusb_free_device_list(items, 1); }
};
std::string location(libusb_device *device) {
    char path[64];
    std::snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u",
                  libusb_get_bus_number(device), libusb_get_device_address(device));
    return path;
}
std::string serial_number(libusb_device *device) {
    // sysfs exposes the descriptor string without issuing USB control transfers.
    std::array<uint8_t, 8> ports{};
    int count = libusb_get_port_numbers(device, ports.data(), static_cast<int>(ports.size()));
    if (count <= 0) return {};
    std::string path = "/sys/bus/usb/devices/" + std::to_string(libusb_get_bus_number(device)) + "-";
    for (int i = 0; i < count; ++i) {
        if (i) path += ".";
        path += std::to_string(ports[static_cast<size_t>(i)]);
    }
    std::ifstream file(path + "/serial"); std::string serial;
    std::getline(file, serial); return serial;
}
bool matches(libusb_device *device) {
    libusb_device_descriptor descriptor{};
    return libusb_get_device_descriptor(device, &descriptor) == 0 &&
           descriptor.idVendor == 0x18e1 && descriptor.idProduct == 0x0104;
}
class Usb final : public Transport {
    struct Input {
        Usb *owner = nullptr;
        libusb_transfer *transfer = nullptr;
        std::array<uint8_t, 8192> buffer{};
        bool interrupt = false;
        std::atomic<bool> pending{false};
    };
public:
    explicit Usb(const std::string &serial, Trace trace) : trace_(std::move(trace)) {
        try { acquire(serial); } catch (...) { cleanup(); throw; }
    }
    ~Usb() override { try { stop(); } catch (...) {} }
    void start(Receiver receiver, Failure failure) override {
        if (started_ || closed_) throw Error(ERR_FAILED, "transport cannot be restarted");
        receiver_ = std::move(receiver); failure_ = std::move(failure); started_ = true;
        try {
            create_attempted_ = true;
            control(1);
            for (auto *input : {&bulk_, &interrupt_}) {
                input->owner = this;
                input->transfer = libusb_alloc_transfer(0);
                if (!input->transfer) throw std::bad_alloc();
                if (input == &bulk_) {
                    libusb_fill_bulk_transfer(input->transfer, handle_, 0x82, input->buffer.data(),
                                             8192, callback, input, 0);
                } else {
                    input->interrupt = true;
                    libusb_fill_interrupt_transfer(input->transfer, handle_, 0x83, input->buffer.data(),
                                                  8, callback, input, 0);
                }
                check(libusb_submit_transfer(input->transfer), "submitting USB input");
                input->pending = true;
            }
            worker_ = std::thread([this] { pump(); });
        } catch (...) { try { stop(); } catch (...) {} throw; }
    }
    void send(std::span<const uint8_t> data, unsigned timeout) override {
        std::lock_guard io(io_mutex_);
        if (stopping_ || !started_) throw Error(ERR_DEVICE_NOT_CONNECTED, "USB transport stopped");
        if (data.size() > 65536 || data.empty() || timeout == 0)
            throw std::invalid_argument("invalid USB write size or timeout");
        emit("OUT", data);
        int transferred = 0;
        const int result = libusb_bulk_transfer(handle_, 0x01, const_cast<uint8_t *>(data.data()),
                                               static_cast<int>(data.size()), &transferred, timeout);
        check(result, "bulk OUT (write not retried)");
        if (transferred != static_cast<int>(data.size()))
            throw Error(ERR_FAILED, "short USB write; command delivery is ambiguous");
    }
    void stop() override {
        std::lock_guard guard(stop_mutex_);
        if (closed_) return;
        stopping_ = true;
        if (worker_.joinable()) worker_.join();
        else drain();
        std::lock_guard io(io_mutex_);
        std::string failure;
        if (create_attempted_ && handle_) {
            try { control(0); } catch (const std::exception &e) { failure = e.what(); }
        }
        const auto release_failure = cleanup();
        closed_ = true;
        if (!release_failure.empty()) { if (!failure.empty()) failure += "; "; failure += release_failure; }
        if (!failure.empty()) throw Error(ERR_FAILED, "cleanup: " + failure);
    }
private:
    void emit(const std::string &kind, std::span<const uint8_t> bytes) {
        if (trace_) trace_(kind, bytes);
    }
    void control(uint16_t value) {
        const Bytes setup{0x40, 0xdb, static_cast<uint8_t>(value), 0, 0, 0, 0, 0};
        emit("CONTROL", setup);
        const int result = libusb_control_transfer(handle_, 0x40, 0xdb, value, 0, nullptr, 0, 1000);
        const std::string outcome = "CONTROL_RESULT_" + std::to_string(result);
        emit(outcome, {});
        check(result, "vendor 0xdb control request");
    }
    void acquire(const std::string &serial) {
        DeviceList devices(context_.value);
        libusb_device *selected = nullptr;
        unsigned candidates = 0;
        for (ssize_t i = 0; i < devices.count; ++i) {
            auto *device = devices.items[i];
            if (!matches(device) || (!serial.empty() && serial_number(device) != serial)) continue;
            selected = device; ++candidates;
        }
        if (!candidates) throw Error(ERR_DEVICE_NOT_CONNECTED, "no matching MongoosePro JLR USB device");
        if (candidates != 1) throw Error(ERR_NOT_UNIQUE, "multiple adapters match; specify a unique serial");
        const auto path = location(selected);
        lock_fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (lock_fd_ < 0) throw Error(ERR_FAILED, "cannot open " + path + ": " + std::strerror(errno) + "; install the narrow udev rule");
        if (flock(lock_fd_, LOCK_EX | LOCK_NB) < 0)
            throw Error(ERR_DEVICE_IN_USE, "adapter is locked by another native driver instance");
        check(libusb_open(selected, &handle_), "opening adapter");
        libusb_config_descriptor *raw = nullptr;
        check(libusb_get_active_config_descriptor(selected, &raw), "reading active USB configuration");
        std::unique_ptr<libusb_config_descriptor, decltype(&libusb_free_config_descriptor)> config(raw, libusb_free_config_descriptor);
        std::set<int> interfaces;
        bool in = false, out = false, intr = false;
        for (uint8_t i = 0; i < config->bNumInterfaces; ++i) {
            const auto &iface = config->interface[i];
            for (int a = 0; a < iface.num_altsetting; ++a) {
                const auto &alt = iface.altsetting[a];
                if (alt.bAlternateSetting != 0) continue;
                for (uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                    const auto &ep = alt.endpoint[e];
                    const auto type = ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                    bool relevant = false;
                    if (ep.bEndpointAddress == 0x82 && type == LIBUSB_TRANSFER_TYPE_BULK) in = relevant = true;
                    if (ep.bEndpointAddress == 0x01 && type == LIBUSB_TRANSFER_TYPE_BULK) out = relevant = true;
                    if (ep.bEndpointAddress == 0x83 && type == LIBUSB_TRANSFER_TYPE_INTERRUPT) intr = relevant = true;
                    if (relevant) interfaces.insert(alt.bInterfaceNumber);
                }
            }
        }
        if (!in || !out || !intr) throw Error(ERR_NOT_SUPPORTED, "USB endpoints differ from researched adapter");
        // Explicit detachment lets cleanup report reattachment failures.
        for (int iface : interfaces) {
            const int active = libusb_kernel_driver_active(handle_, iface);
            check(active, "checking kernel interface owner");
            if (active) {
                check(libusb_detach_kernel_driver(handle_, iface), "detaching CDC interface");
                detached_.push_back(iface);
            }
        }
        for (int iface : interfaces) {
            check(libusb_claim_interface(handle_, iface), "claiming USB interface");
            claimed_.push_back(iface);
        }
    }
    static void LIBUSB_CALL callback(libusb_transfer *transfer) noexcept {
        auto &input = *static_cast<Input *>(transfer->user_data);
        auto &self = *input.owner;
        input.pending = false;
        try {
            if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
                auto bytes = std::span<const uint8_t>(input.buffer.data(), static_cast<size_t>(transfer->actual_length));
                self.emit(input.interrupt ? "INTERRUPT" : "IN", bytes);
                if (!input.interrupt && !bytes.empty()) self.receiver_(bytes);
            } else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
                throw Error(ERR_DEVICE_NOT_CONNECTED, "USB input transfer status " + std::to_string(transfer->status));
            }
            if (!self.stopping_) {
                check(libusb_submit_transfer(transfer), "resubmitting USB input");
                input.pending = true;
            }
        } catch (const std::exception &e) { self.fault(e.what()); }
        catch (...) { self.fault("unknown USB callback failure"); }
    }
    void fault(const std::string &reason) noexcept {
        stopping_ = true;
        try { if (failure_) failure_(reason); } catch (...) {}
    }
    void cancel() {
        for (auto *input : {&bulk_, &interrupt_})
            if (input->pending) libusb_cancel_transfer(input->transfer);
    }
    void drain() {
        cancel();
        while (bulk_.pending || interrupt_.pending) {
            timeval timeout{0, 100000};
            const int result = libusb_handle_events_timeout(context_.value, &timeout);
            if (result < 0 && result != LIBUSB_ERROR_INTERRUPTED) {
                fault("USB event failure while draining");
                // Give up waiting, but leave pending as-is: libusb has not confirmed either
                // transfer is done, only that we stopped being able to ask. cleanup() leaks
                // rather than frees any transfer still in this state, since libusb may
                // complete it later and write into memory we would otherwise have freed.
                drain_failed_ = true;
                break;
            }
        }
    }
    void pump() {
        while (!stopping_) {
            timeval timeout{0, 100000};
            const int result = libusb_handle_events_timeout(context_.value, &timeout);
            if (result < 0 && result != LIBUSB_ERROR_INTERRUPTED) fault("USB event loop failed");
        }
        drain();
    }
    std::string cleanup() noexcept {
        // Never free submitted transfers; stop() drains completion callbacks first. If the
        // drain failed to confirm that (drain_failed_), a transfer still marked pending is
        // intentionally leaked rather than freed.
        for (auto *input : {&bulk_, &interrupt_}) {
            if (input->transfer && !(drain_failed_ && input->pending)) {
                libusb_free_transfer(input->transfer); input->transfer = nullptr;
            }
        }
        std::string failure;
        if (handle_) {
            for (auto i = claimed_.rbegin(); i != claimed_.rend(); ++i) {
                int result = libusb_release_interface(handle_, *i);
                if (result < 0 && result != LIBUSB_ERROR_NO_DEVICE) failure = "USB interface release failed";
            }
            for (auto i = detached_.rbegin(); i != detached_.rend(); ++i) {
                int result = libusb_attach_kernel_driver(handle_, *i);
                if (result < 0 && result != LIBUSB_ERROR_NO_DEVICE) failure = "CDC kernel driver reattachment failed";
            }
            libusb_close(handle_); handle_ = nullptr;
        }
        claimed_.clear(); detached_.clear();
        if (lock_fd_ >= 0) { ::close(lock_fd_); lock_fd_ = -1; }
        return failure;
    }
    Context context_;
    libusb_device_handle *handle_ = nullptr;
    int lock_fd_ = -1;
    std::vector<int> claimed_, detached_;
    Trace trace_;
    Receiver receiver_;
    Failure failure_;
    Input bulk_, interrupt_;
    std::thread worker_;
    std::mutex stop_mutex_, io_mutex_;
    std::atomic<bool> stopping_{false};
    bool started_ = false, closed_ = false, create_attempted_ = false, drain_failed_ = false;
};
}
std::vector<DeviceInfo> usb_devices() {
    Context context; DeviceList devices(context.value);
    std::vector<DeviceInfo> result;
    for (ssize_t i = 0; i < devices.count; ++i) {
        if (!matches(devices.items[i])) continue;
        DeviceInfo info{serial_number(devices.items[i]), location(devices.items[i]), {}};
        if (::access(info.location.c_str(), R_OK | W_OK) != 0) info.error = std::strerror(errno);
        result.push_back(std::move(info));
    }
    return result;
}
std::unique_ptr<Transport> usb_transport(const std::string &serial, Trace trace) {
    return std::make_unique<Usb>(serial, std::move(trace));
}
}
