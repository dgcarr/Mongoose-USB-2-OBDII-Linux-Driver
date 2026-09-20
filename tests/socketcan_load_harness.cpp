// Link-time transport injection for a load build of mongoose-socketcan: the real library (j2534.cpp, Session,
// CanReceiver, the tty reader thread and line discipline) driven over a pty by a simulated adapter, because the
// real one needs a vehicle to produce traffic and the tty backend rightly refuses any node that is not a
// MongoosePro. It answers every command and then streams captured CAN frames at a set rate.
//
//   MONGOOSE_LOAD_RATE    frames per second to stream (default 2450, the rate measured on the car)
//   MONGOOSE_LOAD_TOTAL   how many frames to stream before going quiet (default 5000)
//   MONGOOSE_LOAD_HANGUP  close the pty after this many milliseconds, as an unplug does (default: never)
//
// It streams only what the fixture holds, so every frame the bridge delivers can be checked against it.
#include "session.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
namespace mongoose {
namespace {
unsigned setting(const char *name, unsigned fallback) {
    const char *text = std::getenv(name);
    if (!text || !*text) return fallback;
    const long value = std::strtol(text, nullptr, 10);
    return value > 0 ? static_cast<unsigned>(value) : fallback;
}
std::vector<Bytes> fixture_frames() {
    std::ifstream file(std::string(FIXTURE_DIR) + "/can-receive.frames");
    std::vector<Bytes> frames;
    for (std::string line; std::getline(file, line);)
        if (!line.empty() && line[0] != '#') frames.push_back(unhex(line));
    return frames;
}
void write_all(int fd, const Bytes &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t wrote = ::write(fd, data.data() + sent, data.size() - sent);
        if (wrote > 0) { sent += static_cast<size_t>(wrote); continue; }
        if (wrote < 0 && (errno == EINTR || errno == EAGAIN)) { std::this_thread::sleep_for(std::chrono::microseconds(200)); continue; }
        return;  // the far end is gone; the bridge is expected to notice
    }
}
// A response carries the request's opcode with 0x8000 set, its sequence, status 0 and a device timestamp. Its
// source is whatever the command addressed: the board for device commands, the channel node (0x0501 for CAN) for
// channel ones, which is what Session matches a response against.
Bytes response_for(std::span<const uint8_t> command) {
    const auto opcode = le16(command, 4);
    // A table add (filters, periodic messages) is answered with an opaque firmware handle at response+20.
    static uint32_t handle = 0;
    Bytes body(opcode == 0x12 || opcode == 0xd ? 24 : 20, 0);
    put16(body, 2, le16(command, 0));
    put16(body, 4, static_cast<uint16_t>(opcode | 0x8000));
    put16(body, 6, le16(command, 6));
    if (body.size() == 24) put16(body, 20, static_cast<uint16_t>(++handle));
    return body;
}
// The adapter: answer commands, and once a channel is open and filtered, stream the fixture's frames.
void simulate(int master, std::atomic<bool> &running) {
    const auto frames = fixture_frames();
    const unsigned rate = setting("MONGOOSE_LOAD_RATE", 2450), total = setting("MONGOOSE_LOAD_TOTAL", 5000);
    const unsigned hangup_ms = setting("MONGOOSE_LOAD_HANGUP", 0);
    const auto started = std::chrono::steady_clock::now();
    Decoder decoder;
    bool streaming = false;
    unsigned sent = 0;
    std::vector<uint8_t> chunk(4096);
    while (running) {
        if (hangup_ms && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(hangup_ms)) {
            ::close(master);  // the reader sees EOF, as it does when the adapter is unplugged
            return;
        }
        const ssize_t got = ::read(master, chunk.data(), chunk.size());
        if (got > 0)
            for (const auto &command : decoder.feed(std::span(chunk.data(), static_cast<size_t>(got)))) {
                write_all(master, encode(response_for(command)));
                if (le16(command, 4) == 0x12) streaming = true;   // a filter was added: traffic may flow
                if (le16(command, 4) == 7) streaming = false;     // channel closed
            }
        else if (got < 0 && errno != EAGAIN && errno != EINTR) return;
        if (!streaming || sent >= total || frames.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        // Send the batch this slice of wall clock has earned, so the average holds without a timer per frame.
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const auto owed = static_cast<unsigned>(elapsed * rate);
        for (unsigned i = sent; i < owed && i < total; ++i) write_all(master, frames[i % frames.size()]);
        if (owed > sent) sent = owed < total ? owed : total;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}
struct Adapter {
    int master = -1;
    std::atomic<bool> running{true};
    std::thread thread;
    ~Adapter() { running = false; if (thread.joinable()) thread.join(); }
};
Adapter adapter;
}
// Replaces the production backend dispatch in this build only.
std::unique_ptr<Transport> open_transport(const Selector &, Trace) {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (master < 0 || ::grantpt(master) || ::unlockpt(master)) throw Error(ERR_DEVICE_NOT_CONNECTED, "cannot create pty");
    const char *name = ::ptsname(master);
    const int slave = name ? ::open(name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC) : -1;
    if (slave < 0) throw Error(ERR_DEVICE_NOT_CONNECTED, "cannot open pty slave");
    adapter.master = master;
    adapter.thread = std::thread(simulate, master, std::ref(adapter.running));
    return tty_transport_from_fd(slave, "pty");
}
}
