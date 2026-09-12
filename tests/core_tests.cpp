#include "session.hpp"
#include "replay.hpp"
#include <algorithm>
#include <future>
#include <iostream>
#include <sstream>
#include <random>
#include <atomic>
using namespace mongoose;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("check failed: ") + #x + " at " + std::to_string(__LINE__)); } while (0)
template<class F> void error(int32_t code, F action) {
    try { action(); } catch (const Error &e) { CHECK(e.code == code); return; }
    throw std::runtime_error("expected Error");
}
Bytes response(uint16_t opcode, uint16_t seq, size_t length = 20) {
    Bytes body(length, 0); put16(body, 2, 1); put16(body, 4, opcode); put16(body, 6, seq); return encode(body);
}
struct Mock : Transport {
    Receiver receive;
    Failure failure;
    std::function<void(std::span<const uint8_t>)> write;
    std::atomic<bool> stopped{false};
    unsigned last_timeout = 0;
    void start(Receiver r, Failure f) override { receive = std::move(r); failure = std::move(f); }
    void send(std::span<const uint8_t> data, unsigned timeout) override {
        last_timeout = timeout; if (write) write(data);
    }
    void stop() override { stopped = true; }
};
void codec_tests() {
    const auto body = request(3, 1);
    const auto wire = encode(body);
    CHECK(hex(wire) == "0c00ea51010000000300010000000000");
    for (size_t split = 0; split <= wire.size(); ++split) {
        Decoder decoder;
        auto a = decoder.feed(std::span(wire).first(split));
        auto b = decoder.feed(std::span(wire).subspan(split));
        a.insert(a.end(), b.begin(), b.end());
        CHECK(a.size() == 1 && a[0] == body && decoder.buffered() == 0);
    }
    Bytes combined = wire; combined.insert(combined.end(), wire.begin(), wire.end());
    Decoder decoder; CHECK(decoder.feed(combined).size() == 2);
    Bytes bad{0,0,0,0,0xff,0xff,0x19,0xae}; bad.insert(bad.end(), wire.begin(), wire.end());
    Decoder resync; auto recovered = resync.feed(bad); CHECK(recovered.size() == 1 && recovered[0] == body); CHECK(resync.discarded() == 8);
    Decoder partial; CHECK(partial.feed(std::span(wire).first(wire.size()-1)).empty()); CHECK(partial.buffered() == wire.size()-1);
    CHECK(partial.feed(std::span(wire).last(1))[0] == body);
    Bytes maximum(max_body, 0x55); Decoder large; CHECK(large.feed(encode(maximum))[0] == maximum);
    bool rejected = false; try { encode(Bytes(max_body+1)); } catch (const std::invalid_argument &) { rejected = true; } CHECK(rejected);
    rejected = false; try { encode({}); } catch (const std::invalid_argument &) { rejected = true; } CHECK(rejected);
    Decoder bounded; CHECK(bounded.feed(Bytes(100000, 0xff)).empty()); CHECK(bounded.buffered() < 4);
    std::mt19937 random(42);
    for (unsigned i = 0; i < 300; ++i) {
        Bytes data(1 + random() % max_body);
        for (auto &byte : data) byte = static_cast<uint8_t>(random());
        auto packet = encode(data); Decoder random_decoder; std::vector<Bytes> frames;
        for (size_t pos = 0; pos < packet.size();) {
            auto count = std::min(packet.size()-pos, static_cast<size_t>(1+random()%100));
            auto next = random_decoder.feed(std::span(packet).subspan(pos, count));
            frames.insert(frames.end(), next.begin(), next.end()); pos += count;
        }
        CHECK(frames.size() == 1 && frames[0] == data);
    }
}
void session_tests() {
    auto source = std::make_unique<Mock>(); auto *mock = source.get(); Session session(std::move(source));
    mock->write = [&](auto wire) {
        const auto seq = le16(wire, 10);
        auto wrong_route = response(0x8003, seq); put16(wrong_route, 4, 1); mock->receive(wrong_route);
        mock->receive(response(0x8003, static_cast<uint16_t>(seq+1)));
        mock->receive(response(9, seq)); mock->receive(response(10, seq));
        mock->receive(response(0x8003, seq, 12)); // too short for ordinary response
        auto general = response(1, seq); mock->receive(std::span(general).first(7)); mock->receive(std::span(general).subspan(7));
    };
    CHECK(le16(session.command(3), 4) == 1); // do not require opcode|0x8000
    mock->write = [&](auto wire) { mock->receive(response(0x8100, le16(wire, 10), 12)); };
    CHECK(session.command(0x100).size() == 12);
    mock->write = [&](auto wire) { mock->receive(response(0x8003, le16(wire, 10))); };
    for (int i = 0; i < 253; ++i) CHECK(Session::status(session.command(3)) == 0);
    error(ERR_EXCEEDED_LIMIT, [&] { session.command(3); });
    session.close(); CHECK(mock->stopped); session.close();
    error(ERR_DEVICE_NOT_CONNECTED, [&] { session.command(3); });
}
void timeout_and_cancel() {
    auto source = std::make_unique<Mock>(); auto *mock = source.get(); Session session(std::move(source));
    error(ERR_TIMEOUT, [&] { session.command(3, {}, 3ms); });
    mock->receive(response(0x8003, 1));
    error(ERR_DEVICE_NOT_CONNECTED, [&] { session.command(3); });
    session.close();
    auto pending_source = std::make_unique<Mock>(); auto *pending = pending_source.get();
    Session cancellable(std::move(pending_source)); std::promise<void> sent;
    pending->write = [&](auto) { sent.set_value(); };
    auto operation = std::async(std::launch::async, [&] {
        error(ERR_DEVICE_NOT_CONNECTED, [&] { cancellable.command(3); });
    });
    sent.get_future().wait(); cancellable.close();
    CHECK(operation.wait_for(1s) == std::future_status::ready); operation.get(); CHECK(pending->stopped);
    auto broken_source = std::make_unique<Mock>(); auto *broken = broken_source.get();
    Session disconnected(std::move(broken_source));
    broken->write = [&](auto) { broken->failure("unplugged"); };
    error(ERR_DEVICE_NOT_CONNECTED, [&] { disconnected.command(3); });
    auto short_source = std::make_unique<Mock>(); auto *short_write = short_source.get();
    Session partial(std::move(short_source));
    short_write->write = [&](auto) { throw Error(ERR_FAILED, "short write"); };
    error(ERR_FAILED, [&] { partial.command(3); });
    error(ERR_DEVICE_NOT_CONNECTED, [&] { partial.command(3); });
}
void write_failure_paths() {
    // The smallest accepted budget must still reach the wire. The remainder of a 1 ms
    // request is strictly under a millisecond, so it has to round up: zero is out of
    // contract for every transport -- libusb reads it as "no timeout" and poll(2) as
    // "expire immediately" -- and neither is what an unexpired caller asked for.
    auto source = std::make_unique<Mock>(); auto *mock = source.get(); Session session(std::move(source));
    bool wrote = false;
    mock->write = [&](auto wire) { wrote = true; mock->receive(response(0x8003, le16(wire, 10))); };
    CHECK(Session::status(session.command(3, {}, 1ms)) == 0);
    CHECK(wrote);
    CHECK(mock->last_timeout >= 1); // the transport contract: never hand a backend zero
    CHECK(Session::status(session.command(3)) == 0); // and the session is not poisoned
    // The remaining pre-write expiry branch (deadline genuinely passed before the write)
    // needs a lock-acquisition race to reach, so it is not asserted deterministically
    // here; what it must never do is set failure_, since nothing reached the wire.
    // A root cause reported by the transport outranks the generic write-failure text.
    auto broken_source = std::make_unique<Mock>(); auto *broken = broken_source.get();
    Session unplugged(std::move(broken_source));
    broken->write = [&](auto) {
        broken->failure("adapter unplugged mid-command");
        throw Error(ERR_DEVICE_NOT_CONNECTED, "no device");
    };
    error(ERR_DEVICE_NOT_CONNECTED, [&] { unplugged.command(3); });
    try { unplugged.command(3); CHECK(false); }
    catch (const Error &e) { CHECK(std::string(e.what()).find("unplugged mid-command") != std::string::npos); }
}
void replay_tests() {
    const std::string fixture = "# synthetic, not a hardware capture\nOUT " + hex(encode(request(3,1))) + "\nIN " + hex(response(0x8003,1)) + "\n";
    std::istringstream input(fixture); auto replay = Replay::read(input); auto *view = replay.get();
    Session session(std::move(replay)); CHECK(Session::status(session.command(3)) == 0); view->assert_finished();
    std::istringstream invalid("IN 0000\n"); bool rejected = false;
    try { Replay::read(invalid); } catch (const std::invalid_argument &) { rejected = true; } CHECK(rejected);
    std::istringstream wrong(fixture); Session mismatch(Replay::read(wrong));
    error(ERR_FAILED, [&] { mismatch.command(0x109); });
}
void concurrent_commands() {
    auto source = std::make_unique<Mock>(); auto *mock = source.get(); Session session(std::move(source));
    std::atomic<int> active{0};
    mock->write = [&](auto wire) {
        CHECK(++active == 1); mock->receive(response(0x8003, le16(wire, 10))); --active;
    };
    std::vector<std::future<void>> operations;
    for (int i = 0; i < 20; ++i) operations.push_back(std::async(std::launch::async, [&] { CHECK(Session::status(session.command(3)) == 0); }));
    for (auto &operation : operations) operation.get();
}
void selector_tests() {
    // Defaults and the historic serial: form, which tools/client.c still passes.
    CHECK(parse_selector("").backend == Backend::Tty);
    CHECK(parse_selector("").serial.empty() && parse_selector("").path.empty());
    CHECK(parse_selector("serial:ABC").backend == Backend::Tty);
    CHECK(parse_selector("serial:ABC").serial == "ABC");
    CHECK(parse_selector("tty:").backend == Backend::Tty);
    CHECK(parse_selector("tty:serial:ABC").serial == "ABC");
    CHECK(parse_selector("tty:/dev/ttyACM3").path == "/dev/ttyACM3");
    CHECK(parse_selector("tty:/dev/ttyACM3").serial.empty());
    CHECK(parse_selector("usb:").backend == Backend::Usb);
    CHECK(parse_selector("usb:serial:ABC").backend == Backend::Usb);
    CHECK(parse_selector("usb:serial:ABC").serial == "ABC");
    // Rejections: unknown prefix, empty serial, a path where no namespace exists,
    // and traversal in a node path.
    for (const char *bad : {"not-a-selector", "serial:", "tty:serial:", "usb:/dev/ttyACM3",
                            "tty:relative", "tty:/dev/../etc/passwd"})
        error(ERR_FAILED, [&] { parse_selector(bad); });
}
int main() {
    try { selector_tests(); codec_tests(); session_tests(); timeout_and_cancel(); write_failure_paths(); replay_tests(); concurrent_commands();
        std::cout << "selectors, codec, correlation, quarantine, cancellation, write failures, replay, concurrency passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
