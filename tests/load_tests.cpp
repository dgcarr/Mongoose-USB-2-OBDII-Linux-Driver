// Sustained-receive harness. The plan's D4 gap is that Linux back-pressure has never
// been measured: the Windows baseline of ~2455 msg/s says nothing about how the tty path
// behaves when the reader lags, because cdc_acm goes through the n_tty flip buffer and
// throttles where the libusb path used two outstanding URBs.
//
// A pty gives that path a load generator without a vehicle. It exercises the real
// tty.cpp reader thread, the real line discipline, the real framing decoder and the real
// CanReceiver queue. It does NOT exercise the cdc_acm URB path, so it bounds what the
// host can absorb; it does not prove parity with the adapter.
#include "session.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace mongoose;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at " + std::to_string(__LINE__)); } while (0)
namespace {
std::vector<Bytes> fixture_frames() {
    std::ifstream file(std::string(FIXTURE_DIR) + "/can-receive.frames");
    CHECK(file.good());
    std::vector<Bytes> frames;
    for (std::string line; std::getline(file, line);)
        if (!line.empty() && line[0] != '#') frames.push_back(unhex(line));
    CHECK(!frames.empty());
    return frames;
}
// Body layout for cInboundData: DataSize at +22, payload at +24 (PROTOCOL.md section 3).
Bytes frame_payload(const Bytes &wire) {
    const auto body = std::span<const uint8_t>(wire).subspan(4);
    const auto size = le16(body, 22);
    return Bytes(body.begin() + 24, body.begin() + 24 + size);
}
struct Pty {
    int master = -1, slave = -1;
    Pty() {
        master = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || ::grantpt(master) || ::unlockpt(master)) throw std::runtime_error("cannot create pty");
        const char *name = ::ptsname(master);
        if (!name) throw std::runtime_error("cannot name pty");
        slave = ::open(name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (slave < 0) throw std::runtime_error("cannot open pty slave");
    }
    ~Pty() { if (master >= 0) ::close(master); }
};
void write_all(int fd, const Bytes &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t wrote = ::write(fd, data.data() + sent, data.size() - sent);
        if (wrote > 0) { sent += static_cast<size_t>(wrote); continue; }
        if (wrote < 0 && (errno == EINTR || errno == EAGAIN)) { std::this_thread::sleep_for(200us); continue; }
        throw std::runtime_error("pty write failed: " + std::string(std::strerror(errno)));
    }
}
// The consumer keeps up, so nothing may be lost, reordered or corrupted. Every received
// message is checked against the fixture frame that produced it.
void sustained_receive(size_t total) {
    const auto frames = fixture_frames();
    Pty pty;
    auto receiver = std::make_shared<CanReceiver>();
    Session session(tty_transport_from_fd(pty.slave, "pty"));
    session.set_can_receiver(receiver);
    std::atomic<size_t> consumed{0};
    std::atomic<bool> overflowed{false}, running{true};
    const auto started = std::chrono::steady_clock::now();
    std::thread consumer([&] {
        std::vector<PASSTHRU_MSG> batch(256);
        while (running || consumed < total) {
            uint32_t count = static_cast<uint32_t>(batch.size());
            try { receiver->read(batch.data(), count, count, 20); }
            catch (const Error &error) {
                if (error.code == ERR_BUFFER_OVERFLOW) overflowed = true;
                if (error.code != ERR_BUFFER_EMPTY && error.code != ERR_TIMEOUT &&
                    error.code != ERR_BUFFER_OVERFLOW) return;
            }
            for (uint32_t i = 0; i < count; ++i) {
                const auto expected = frame_payload(frames[(consumed + i) % frames.size()]);
                CHECK(batch[i].DataSize == expected.size());
                CHECK(std::equal(expected.begin(), expected.end(), batch[i].Data));
            }
            consumed += count;
        }
    });
    for (size_t i = 0; i < total; ++i) {
        write_all(pty.master, frames[i % frames.size()]);
        if ((i + 1) % 20000 == 0) {
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::cout << "  sent " << i + 1 << '/' << total << " consumed " << consumed
                      << " (" << static_cast<unsigned>(static_cast<double>(i + 1) / elapsed) << " msg/s)" << std::endl;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (consumed < total && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
    running = false;
    consumer.join();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::cout << "sustained: " << consumed << '/' << total << " messages in " << elapsed << " s = "
              << static_cast<unsigned>(static_cast<double>(consumed.load()) / elapsed) << " msg/s, overflow=" << overflowed << '\n';
    CHECK(consumed == total);
    CHECK(!overflowed);
    session.close();
}
// A consumer that never reads must not lose data silently: the queue fills and the next
// read reports ERR_BUFFER_OVERFLOW rather than returning a short, gap-ridden batch.
//
// Written in rounds that each add well over a queue's worth while draining only one
// batch, because reading and filling concurrently is a race: under a slow sanitizer
// build the reader drained as fast as the producer filled, the queue never reached
// capacity, and the test failed for the wrong reason.
void overflow_is_reported() {
    const auto frames = fixture_frames();
    Pty pty;
    auto receiver = std::make_shared<CanReceiver>();
    Session session(tty_transport_from_fd(pty.slave, "pty"));
    session.set_can_receiver(receiver);
    bool reported = false;
    for (unsigned round = 0; round < 10 && !reported; ++round) {
        for (size_t i = 0; i < CanReceiver::capacity + 512; ++i) write_all(pty.master, frames[i % frames.size()]);
        // Let the reader thread ingest what was just written without draining it.
        std::this_thread::sleep_for(200ms);
        std::vector<PASSTHRU_MSG> batch(64);
        uint32_t count = 64;
        try { receiver->read(batch.data(), count, count, 20); }
        catch (const Error &error) { if (error.code == ERR_BUFFER_OVERFLOW) reported = true; }
    }
    std::cout << "overflow reported: " << reported << '\n';
    CHECK(reported);
    session.close();
}
// Nothing drains the master, so the slave's output buffer fills and write() goes short.
// tty.cpp's retry loop is the only thing between that and a silently truncated command,
// and docs/VALIDATION.md records it as inspected but never exercised: no read-only
// opcode produces a frame anywhere near the 0x1800 limit against real hardware.
//
// Driven through the Transport directly rather than through Session. A response timeout
// poisons a session permanently, so a loop of commands would die on the first one and
// never reach the write path this is meant to test.
void partial_write_path() {
    Pty pty;
    auto transport = tty_transport_from_fd(pty.slave, "pty");
    transport->start([](std::span<const uint8_t>) {}, [](const std::string &) {});
    const auto maximum = encode(Bytes(max_body, 0x5a));  // the largest frame the codec allows
    size_t completed = 0;
    std::string outcome;
    int32_t code = 0;
    for (unsigned attempt = 0; attempt < 32 && outcome.empty(); ++attempt) {
        try { transport->send(maximum, 50); ++completed; }
        catch (const Error &error) { outcome = error.what(); code = error.code; }
    }
    std::cout << "maximum-size writes completed: " << completed
              << ", then: " << (outcome.empty() ? "no refusal" : outcome) << " (code " << code << ")\n";
    // Either branch proves the retry loop ran against a full buffer: a partial write is
    // reported as ambiguous delivery, and a write that placed no byte at all is a clean
    // timeout that costs only the sequence slot.
    CHECK(completed > 0);
    CHECK(code == ERR_FAILED || code == ERR_TIMEOUT);
    transport->stop();
}
}
int main(int argc, char **argv) {
    try {
        const size_t total = argc > 1 ? std::stoul(argv[1]) : 60000;
        sustained_receive(total);
        overflow_is_reported();
        partial_write_path();
        std::cout << "pty load harness passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
