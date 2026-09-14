#include "transport.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
namespace mongoose {
namespace {
constexpr char vendor_id[] = "18e1";
constexpr char product_id[] = "0104";
constexpr size_t read_chunk = 8192;
int32_t errno_status(int code) {
    if (code == ENODEV || code == ENXIO || code == ENOENT || code == EIO || code == EPIPE)
        return ERR_DEVICE_NOT_CONNECTED;
    if (code == EBUSY) return ERR_DEVICE_IN_USE;
    if (code == ETIMEDOUT) return ERR_TIMEOUT;
    // EACCES and EPERM are permission problems, not contention; describe() adds the hint.
    return ERR_FAILED;
}
// PassThruGetLastError copies into an 80-byte buffer, so the hint stays terse.
std::string describe(int code) {
    std::string text = std::strerror(code);
    if (code == EACCES || code == EPERM) text += "; join group dialout";
    return text;
}
// poll(2) takes an int; clamp so a large caller timeout cannot overflow or round to zero.
int poll_budget(std::chrono::steady_clock::duration left) {
    const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(left).count();
    return static_cast<int>(std::clamp<long long>(milliseconds, 1, 60000));
}
std::string attribute(const std::filesystem::path &path) {
    std::ifstream file(path); std::string value;
    std::getline(file, value); return value;
}
// The USB device directory carrying idVendor/idProduct/serial, reached by walking up
// from the tty's interface directory. sysfs exposes the descriptor strings without
// issuing USB control transfers, exactly as the libusb backend does.
std::filesystem::path usb_directory(const std::string &node) {
    std::error_code code;
    auto path = std::filesystem::canonical("/sys/class/tty/" + node + "/device", code);
    if (code) return {};
    for (int depth = 0; depth < 4 && path.has_parent_path(); ++depth) {
        path = path.parent_path();
        if (std::filesystem::exists(path / "idVendor", code)) return path;
    }
    return {};
}
bool is_adapter(const std::string &node, std::string &serial) {
    const auto directory = usb_directory(node);
    if (directory.empty()) return false;
    // Both ids must match: unrelated CDC-ACM devices routinely share this bus.
    if (attribute(directory / "idVendor") != vendor_id) return false;
    if (attribute(directory / "idProduct") != product_id) return false;
    serial = attribute(directory / "serial");
    return true;
}
struct Candidate { std::string location, serial; };
std::vector<Candidate> candidates() {
    std::vector<Candidate> found;
    std::error_code code;
    std::filesystem::directory_iterator entries("/sys/class/tty", code);
    if (code) return found;
    for (const auto &entry : entries) {
        const auto node = entry.path().filename().string();
        if (node.rfind("ttyACM", 0) != 0) continue;
        std::string serial;
        if (!is_adapter(node, serial)) continue;
        found.push_back({"/dev/" + node, serial});
    }
    std::sort(found.begin(), found.end(),
              [](const Candidate &a, const Candidate &b) { return a.location < b.location; });
    return found;
}
class Tty final : public Transport {
public:
    explicit Tty(const Selector &selector, Trace trace) : trace_(std::move(trace)) {
        try { acquire(selector); } catch (...) { cleanup(); throw; }
    }
    // Adopts an already-open character device. Resolving and validating the adapter is
    // the caller's job on this path; the reader thread, framing and write loop below are
    // the same ones the production factory uses.
    Tty(int fd, const std::string &label, Trace trace) : trace_(std::move(trace)) {
        fd_ = fd;
        try { configure(label); } catch (...) { cleanup(); throw; }
    }
    ~Tty() override { try { stop(); } catch (...) {} }
    void start(Receiver receiver, Failure failure) override {
        if (started_ || closed_) throw Error(ERR_FAILED, "transport cannot be restarted");
        receiver_ = std::move(receiver); failure_ = std::move(failure); started_ = true;
        try {
            wake_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (wake_ < 0) throw Error(errno_status(errno), "eventfd: " + describe(errno));
            worker_ = std::thread([this] { pump(); });
        } catch (...) { try { stop(); } catch (...) {} throw; }
    }
    void send(std::span<const uint8_t> data, unsigned timeout) override {
        if (data.size() > 65536 || data.empty() || timeout == 0)
            throw std::invalid_argument("invalid tty write size or timeout");
        // Fences the write against stop() closing (and the kernel recycling) the fd.
        std::lock_guard io(io_mutex_);
        if (stopping_ || !started_ || fd_ < 0)
            throw Error(ERR_DEVICE_NOT_CONNECTED, "tty transport stopped");
        emit("OUT", data);
        // A short write is normal here, not an error: cdc_acm accepts only a few 64-byte
        // buffers at a time, so a maximum-size frame needs several passes. Failing to
        // finish inside the caller's budget is the error.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t wrote = ::write(fd_, data.data() + sent, data.size() - sent);
            if (wrote > 0) { sent += static_cast<size_t>(wrote); continue; }
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                throw Error(errno_status(errno), "tty write: " + describe(errno));
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) break;
            pollfd waiting{fd_, POLLOUT, 0};
            const int ready = ::poll(&waiting, 1, poll_budget(left));
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) throw Error(errno_status(errno), "tty write poll: " + describe(errno));
            if (ready == 0) break;
            if (waiting.revents & (POLLERR | POLLHUP | POLLNVAL))
                throw Error(ERR_DEVICE_NOT_CONNECTED, "adapter disconnected during write");
        }
        // Nothing on the wire only costs the sequence slot; a partial frame does not.
        if (sent == 0) throw Error(ERR_TIMEOUT, "tty write timed out; no byte reached the adapter");
        if (sent != data.size()) throw Error(ERR_FAILED, "partial tty write; command delivery is ambiguous");
    }
    // Unlike the libusb backend there is no control request to send and no kernel driver
    // to reattach, so cleanup cannot fail and this never throws.
    void stop() override {
        std::lock_guard guard(stop_mutex_);
        if (closed_) return;
        stopping_ = true;
        wake();
        if (worker_.joinable()) worker_.join();
        std::lock_guard io(io_mutex_);
        cleanup();
        closed_ = true;
    }
private:
    void emit(const std::string &kind, std::span<const uint8_t> bytes) {
        if (trace_) trace_(kind, bytes);
    }
    void wake() noexcept {
        if (wake_ < 0) return;
        const uint64_t one = 1;
        const ssize_t ignored = ::write(wake_, &one, sizeof one);
        static_cast<void>(ignored);
    }
    void acquire(const Selector &selector) {
        std::string path = selector.path;
        if (path.empty()) {
            std::vector<Candidate> matched;
            for (const auto &entry : candidates())
                if (selector.serial.empty() || entry.serial == selector.serial) matched.push_back(entry);
            if (matched.empty())
                throw Error(ERR_DEVICE_NOT_CONNECTED, "no matching MongoosePro JLR tty device");
            if (matched.size() != 1)
                throw Error(ERR_NOT_UNIQUE, "multiple adapters match; specify a unique serial");
            path = matched.front().location;
        } else {
            // An explicit path must still be the researched adapter, not a stray modem.
            // Canonicalize to resolve symlinks like /dev/serial/by-id/... to the kernel tty node.
            std::error_code code;
            const auto target = std::filesystem::canonical(path, code);
            const auto node = std::filesystem::path(code ? path : target.string()).filename().string();
            std::string serial;
            if (!is_adapter(node, serial))
                throw Error(ERR_NOT_SUPPORTED, path + " is not a MongoosePro JLR adapter");
        }
        fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) throw Error(errno_status(errno), "cannot open " + path + ": " + describe(errno));
        // flock excludes other instances of this driver deterministically; TIOCEXCL makes
        // a later open() by minicom, screen or a ModemManager probe fail with EBUSY.
        // Neither alone suffices: flock is advisory, TIOCEXCL does not bind root.
        if (::flock(fd_, LOCK_EX | LOCK_NB) < 0)
            throw Error(ERR_DEVICE_IN_USE, "adapter is locked by another driver instance");
        if (::ioctl(fd_, TIOCEXCL) < 0) emit("TIOCEXCL_FAILED", {});
        configure(path);
    }
    void configure(const std::string &path) {
        termios settings{};
        // Built from zero rather than edited: the framing is binary, and any inherited
        // ICANON/ONLCR/IXON would corrupt 0x0a, 0x11 and 0x13 bytes inside payloads.
        settings.c_iflag = 0;
        settings.c_oflag = 0;
        settings.c_lflag = 0;
        settings.c_cflag = static_cast<tcflag_t>(CS8 | CREAD | CLOCAL);
        settings.c_cc[VMIN] = cc_t{0};
        settings.c_cc[VTIME] = cc_t{0};
        if (::cfsetispeed(&settings, B115200) < 0 || ::cfsetospeed(&settings, B115200) < 0)
            throw Error(ERR_FAILED, "cannot set tty line speed on " + path);
        if (::tcsetattr(fd_, TCSANOW, &settings) < 0)
            throw Error(errno_status(errno), "cannot configure " + path + ": " + describe(errno));
        // tcsetattr reports success if it applied *any* requested change, so verify that
        // raw mode actually took: a half-applied line discipline would corrupt payloads.
        termios applied{};
        if (::tcgetattr(fd_, &applied) < 0)
            throw Error(errno_status(errno), "cannot read back tty settings: " + describe(errno));
        const tcflag_t required = static_cast<tcflag_t>(CS8 | CLOCAL);
        if (applied.c_iflag != 0 || applied.c_oflag != 0 || applied.c_lflag != 0 ||
            (applied.c_cflag & required) != required)
            throw Error(ERR_FAILED, "tty did not accept raw mode on " + path);
        ::tcflush(fd_, TCIOFLUSH);
    }
    void fault(const std::string &reason) noexcept {
        stopping_ = true;
        try { if (failure_) failure_(reason); } catch (...) {}
    }
    void pump() noexcept {
        std::array<uint8_t, read_chunk> buffer{};
        while (!stopping_) {
            pollfd waiting[2]{{fd_, POLLIN, 0}, {wake_, POLLIN, 0}};
            const int ready = ::poll(waiting, 2, 100);
            if (ready < 0) {
                if (errno == EINTR) continue;
                fault("tty poll: " + describe(errno));
                return;
            }
            if (ready == 0) continue;
            if (waiting[1].revents & POLLIN) return;  // stop() rang the eventfd
            // CLOCAL is set, so POLLHUP here is a real unplug rather than carrier loss.
            if (waiting[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fault("adapter disconnected");
                return;
            }
            if (!(waiting[0].revents & POLLIN)) continue;
            const ssize_t count = ::read(fd_, buffer.data(), buffer.size());
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                fault("tty read: " + describe(errno));
                return;
            }
            // With VMIN=0/VTIME=0 a zero-length read is legal and is not end of file.
            if (count == 0) continue;
            const auto bytes = std::span<const uint8_t>(buffer.data(), static_cast<size_t>(count));
            try {
                emit("IN", bytes);
                if (receiver_) receiver_(bytes);
            } catch (const std::exception &error) { fault(error.what()); return; }
            catch (...) { fault("unknown tty receive failure"); return; }
        }
    }
    // flock and TIOCEXCL are released implicitly when the descriptor closes.
    void cleanup() noexcept {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (wake_ >= 0) { ::close(wake_); wake_ = -1; }
    }
    int fd_ = -1;
    int wake_ = -1;
    Trace trace_;
    Receiver receiver_;
    Failure failure_;
    std::thread worker_;
    std::mutex stop_mutex_, io_mutex_;
    std::atomic<bool> stopping_{false};
    bool started_ = false, closed_ = false;
};
}
std::vector<DeviceInfo> tty_devices() {
    std::vector<DeviceInfo> result;
    for (const auto &entry : candidates()) {
        DeviceInfo info{entry.serial, entry.location, {}};
        if (::access(info.location.c_str(), R_OK | W_OK) != 0) info.error = std::strerror(errno);
        result.push_back(std::move(info));
    }
    return result;
}
std::unique_ptr<Transport> tty_transport(const Selector &selector, Trace trace) {
    return std::make_unique<Tty>(selector, std::move(trace));
}
std::unique_ptr<Transport> tty_transport_from_fd(int fd, const std::string &label, Trace trace) {
    return std::make_unique<Tty>(fd, label, std::move(trace));
}
}
