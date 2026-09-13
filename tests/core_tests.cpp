#include "session.hpp"
#include "replay.hpp"
#include <algorithm>
#include <future>
#include <iostream>
#include <set>
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
        auto wrong_source = response(0x8003, seq); put16(wrong_source, 6, channel_node(CAN)); mock->receive(wrong_source);
        mock->receive(response(0x8003, static_cast<uint16_t>(seq+1)));
        mock->receive(response(9, seq)); mock->receive(response(10, seq));
        mock->receive(response(0x8003, seq, 12)); // too short for ordinary response
        auto general = response(1, seq); mock->receive(std::span(general).first(7)); mock->receive(std::span(general).subspan(7));
    };
    CHECK(le16(session.command(3), 4) == 1); // do not require opcode|0x8000
    mock->write = [&](auto wire) { mock->receive(response(0x8100, le16(wire, 10), 12)); };
    CHECK(session.command(0x100).size() == 12);
    // A sequence whose response arrived is complete and returns to the pool at once, so
    // a burst far longer than the 255-value space must not be refused. The previous rule
    // held every sequence for ten seconds and failed the 256th command in that window,
    // capping the driver near 25 commands per second -- fine for setup, impossible for
    // sustained transmit. Only abandoned sequences are held now, and every path that
    // abandons one also poisons the session, so exhaustion is unreachable from here.
    std::set<uint16_t> seen;
    mock->write = [&](auto wire) {
        const auto seq = le16(wire, 10);
        CHECK(seq >= 1 && seq <= 255);
        seen.insert(seq); mock->receive(response(0x8003, seq));
    };
    for (int i = 0; i < 1000; ++i) CHECK(Session::status(session.command(3)) == 0);
    CHECK(seen.size() == 255 && *seen.begin() == 1 && *seen.rbegin() == 255);
    CHECK(session.usable());
    session.close(); CHECK(mock->stopped); session.close();
    CHECK(!session.usable());
    error(ERR_DEVICE_NOT_CONNECTED, [&] { session.command(3); });
}
void channel_routing_tests() {
    auto source = std::make_unique<Mock>(); auto *mock = source.get(); Session session(std::move(source));
    mock->write = [&](auto wire) {
        CHECK(le16(wire, 4) == channel_node(CAN));
        const auto seq = le16(wire, 10);
        mock->receive(response(0x8006, seq)); // right sequence, wrong source
        auto other_channel = response(0x8006, seq);
        put16(other_channel, 6, channel_node(ISO15765)); mock->receive(other_channel);
        auto general = response(1, seq);
        put16(general, 6, channel_node(CAN)); mock->receive(general);
    };
    CHECK(le16(session.command(6, {}, 100ms, channel_node(CAN)), 4) == 1);
    mock->write = [&](auto wire) { mock->receive(response(0x8006, le16(wire, 10))); };
    error(ERR_TIMEOUT, [&] { session.command(6, {}, 3ms, channel_node(CAN)); });
    error(ERR_DEVICE_NOT_CONNECTED, [&] { session.command(3); });
}
void timeout_and_cancel() {
    auto source = std::make_unique<Mock>(); auto *mock = source.get(); Session session(std::move(source));
    error(ERR_TIMEOUT, [&] { session.command(3, {}, 3ms); });
    CHECK(!session.usable());
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
// Frames captured from the vendor Windows driver against the real adapter, used
// here as ground truth rather than as anything we generated ourselves. Sources
// are under analysis/captures/windows/; see docs/WINDOWS-FINDINGS.md.
namespace vendor {
// C1/C2: PassThruConnect. Body is u32 ConnectFlags then u32 baud.
constexpr auto open_channel_iso15765_500k =
    "1400f2510106000006000800000000000000000020a10700";
constexpr auto open_channel_can_500k =
    "1400f2510105000006000800000019770000000020a10700";
constexpr auto open_channel_can_250k =
    "1400f2510105000006000800000019770000000090d00300";
constexpr auto open_channel_can_500k_29bit =
    "1400f2510105000006000800000019770001000020a10700";
// C1: the pin routing that follows every Connect - (1, 6, 14), CAN High and Low.
constexpr auto set_pin_can =
    "1800fe5101050000120009000000147701000000060000000e000000";
// B2: cGetString selector 0 and its response carrying the serial.
constexpr auto get_string_serial_response =
    "2900cf510000010013800a00000035000000000090c41e0000000000414f4c4845303030303030333636364100";
// B1: cOpenDevice response. Status 0 and a microsecond counter of 0, because
// cOpenDevice is what resets that counter.
constexpr auto open_device_response =
    "1500f351000001000380060000008f00000000000000000000";
}

// The vendor leaves body+10 uninitialised and the firmware echoes it back; we
// always send zero. Comparing against captured frames therefore means masking
// that one field, and asserting it is the *only* difference.
Bytes token_cleared(const std::string &frame) {
    auto bytes = unhex(frame);
    CHECK(bytes.size() >= 4 + 12);
    put16(bytes, 4 + 10, 0);
    return bytes;
}

void vendor_frame_tests() {
    // Our builder reproduces the vendor's bytes exactly, once the echoed token is
    // masked. The ISO15765 capture happens to carry a zero token already, so that
    // one is a byte-for-byte match with no masking at all.
    const Bytes flags_zero_baud_500k = unhex("0000000020a10700");
    CHECK(encode(request(0x06, 8, flags_zero_baud_500k, channel_node(6)))
          == unhex(vendor::open_channel_iso15765_500k));

    CHECK(encode(request(0x06, 8, flags_zero_baud_500k, channel_node(5)))
          == token_cleared(vendor::open_channel_can_500k));
    CHECK(encode(request(0x06, 8, unhex("0000000090d00300"), channel_node(5)))
          == token_cleared(vendor::open_channel_can_250k));
    // CAN_29BIT_ID is 0x100, passed through into the flags word verbatim.
    CHECK(encode(request(0x06, 8, unhex("0001000020a10700"), channel_node(5)))
          == token_cleared(vendor::open_channel_can_500k_29bit));
    CHECK(encode(request(0x12, 9, unhex("01000000060000000e000000"), channel_node(5)))
          == token_cleared(vendor::set_pin_can));

    // Node addressing, stated as the rule rather than as three magic numbers.
    CHECK(channel_node(5) == 0x0501);
    CHECK(channel_node(6) == 0x0601);
    CHECK(le16(unhex(vendor::open_channel_can_500k), 4) == channel_node(5));
    CHECK(le16(unhex(vendor::open_channel_iso15765_500k), 4) == channel_node(6));
    // Device-level commands stay addressed to the board itself.
    CHECK(le16(encode(request(3, 1)), 4) == board_node);
    // Node 0 is the PC. Addressing a request to it is a programming error, and
    // request() rejects it the same way it rejects a bad size or sequence.
    bool rejected = false;
    try { request(3, 1, {}, 0); } catch (const std::invalid_argument &) { rejected = true; }
    CHECK(rejected);

    // Real vendor responses must survive the decoder, including across arbitrary
    // USB chunk boundaries, and parse to the documented fields.
    for (const char *frame : {vendor::open_channel_can_500k, vendor::set_pin_can,
                              vendor::get_string_serial_response, vendor::open_device_response}) {
        const auto wire = unhex(frame);
        for (size_t split = 0; split <= wire.size(); ++split) {
            Decoder decoder;
            auto a = decoder.feed(std::span(wire).first(split));
            auto b = decoder.feed(std::span(wire).subspan(split));
            a.insert(a.end(), b.begin(), b.end());
            CHECK(a.size() == 1 && decoder.buffered() == 0);
            CHECK(a[0] == Bytes(wire.begin() + 4, wire.end()));
        }
    }

    // A response swaps dst and src, and sets the high bit of the opcode.
    const auto open_channel_response = unhex("1500f351000001050680080000001977000000004cc71e0000");
    Decoder decoder;
    const auto frames = decoder.feed(open_channel_response);
    CHECK(frames.size() == 1);
    const auto &body = frames[0];
    CHECK(le16(body, 0) == 0);                    // dst: back to the PC
    CHECK(le16(body, 2) == channel_node(5));      // src: the CAN channel node
    CHECK(le16(body, 4) == (0x06 | 0x8000));      // response bit
    CHECK(le16(body, 6) == 8);                    // sequence echoed
    CHECK(le16(body, 10) == 0x7719);              // token echoed verbatim
    CHECK(Session::status(body) == 0);

    // cOpenDevice resets the device microsecond counter, so its own response
    // reports zero. This is the origin of J2534 timestamps.
    Decoder opened;
    const auto open_frames = opened.feed(unhex(vendor::open_device_response));
    CHECK(open_frames.size() == 1);
    CHECK(Session::status(open_frames[0]) == 0);
    CHECK(le32(open_frames[0], 16) == 0);

    // The serial in the cGetString response matches the adapter's USB iSerial.
    Decoder strings;
    const auto string_frames = strings.feed(unhex(vendor::get_string_serial_response));
    CHECK(string_frames.size() == 1);
    const auto &serial_body = string_frames[0];
    const std::string serial(serial_body.begin() + 24, serial_body.end() - 1);
    CHECK(serial == "AOLHE0000003666A");
}

int main() {
    try { selector_tests(); codec_tests(); session_tests(); channel_routing_tests(); timeout_and_cancel(); write_failure_paths(); replay_tests(); concurrent_commands(); vendor_frame_tests();
        std::cout << "selectors, codec, correlation, sequence reuse, cancellation, write failures, replay, concurrency, vendor frames passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
