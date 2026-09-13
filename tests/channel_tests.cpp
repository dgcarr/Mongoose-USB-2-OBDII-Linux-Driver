#include "replay.hpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <future>
#include <iostream>
#include <cstring>
#include <utility>
#include <array>
#include <fstream>
#include <sstream>
using namespace mongoose;
using namespace std::chrono_literals;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at " + std::to_string(__LINE__)); } while (0)
namespace {
enum class Outcome { Reply, Timeout, ShortWrite, Unplug };
struct Step {
    Exchange exchange;
    Outcome outcome = Outcome::Reply;
    std::function<void()> before_reply;
};
struct Script {
    Receiver receive;
    Failure fail;
    std::deque<Step> steps;
    uint16_t sequence = 0;
    bool stopped = false, mismatch = false;
    void add(uint16_t opcode, Bytes payload = {}, uint16_t node = board_node, uint16_t status = 0,
             uint16_t chan = 0) {
        ++sequence;
        Bytes body(20, 0);
        put16(body, 2, node); put16(body, 4, opcode | 0x8000); put16(body, 6, sequence); put16(body, 12, status);
        steps.push_back({{encode(request(opcode, sequence, payload, node, chan)), {encode(body)}}, Outcome::Reply, {}});
    }
    // C1/C2 vendor requests and C1 replies, captured in
    // analysis/captures/windows/20260913T161048-c1-connect-can-500k/wire.pcap.
    // Normalize ONLY sequence (Linux has a shorter startup) and opaque token.
    // Timestamps and trailing response bytes remain exactly as captured.
    void captured(const char *out, const char *in) {
        auto request_wire = unhex(out), response_wire = unhex(in);
        ++sequence;
        for (auto *wire : {&request_wire, &response_wire}) {
            put16(*wire, 10, sequence); put16(*wire, 14, 0);
        }
        steps.push_back({{request_wire, {response_wire}}, Outcome::Reply, {}});
    }
    void connect(uint32_t flags = 0, uint32_t baud = 500000) {
        const char *wire = flags ? "1400f2510105000006000800000019770001000020a10700" :
            baud == 250000 ? "1400f2510105000006000800000019770000000090d00300" :
                            "1400f2510105000006000800000019770000000020a10700";
        captured(wire, "1500f351000001050680080000001977000000004cc71e0000");
        captured("1800fe5101050000120009000000147701000000060000000e000000",
                 "1500f3510000010512800900000014770000000078c81e0000");
    }
    void disconnect() {
        captured("0c00ea510105000007000a0000000000", "1500f3510000010507800a000000000000000000747b3d0000");
    }
    void finished() const { CHECK(steps.empty()); CHECK(stopped); CHECK(!mismatch); }
};
std::shared_ptr<Script> next_script;
class ScriptTransport final : public Transport {
    std::shared_ptr<Script> script_;
    Receiver receiver_;
    Failure failure_;
public:
    explicit ScriptTransport(std::shared_ptr<Script> script) : script_(std::move(script)) {}
    void start(Receiver receiver, Failure failure) override {
        script_->receive = receiver; script_->fail = failure;
        receiver_ = std::move(receiver); failure_ = std::move(failure);
    }
    void send(std::span<const uint8_t> wire, unsigned timeout) override {
        CHECK(timeout > 0);
        if (script_->steps.empty() || !std::equal(wire.begin(), wire.end(), script_->steps.front().exchange.out.begin(), script_->steps.front().exchange.out.end())) {
            script_->mismatch = true;
            throw Error(ERR_FAILED, "unexpected wire command");
        }
        auto step = std::move(script_->steps.front()); script_->steps.pop_front();
        if (step.before_reply) step.before_reply();
        if (step.outcome == Outcome::Timeout) return; // real Session response timeout
        if (step.outcome == Outcome::ShortWrite) throw Error(ERR_FAILED, "partial channel write");
        if (step.outcome == Outcome::Unplug) { failure_("adapter unplugged during channel command"); return; }
        for (const auto &chunk : step.exchange.in) receiver_(chunk);
    }
    void stop() override { script_->stopped = true; }
};
std::shared_ptr<Script> prepare() {
    auto script = std::make_shared<Script>();
    for (uint16_t op : std::array<uint16_t, 3>{0x103, 3, 0x109}) script->add(op);
    next_script = script;
    return script;
}
uint32_t open() { uint32_t id = 0; CHECK(PassThruOpen(nullptr, &id) == 0); CHECK(id); return id; }
uint32_t connect(uint32_t device, uint32_t flags = 0, uint32_t baud = 500000) {
    uint32_t id = 0; CHECK(PassThruConnect(device, CAN, flags, baud, &id) == 0); CHECK(id > device); return id;
}
void unsupported_operations(uint32_t channel, int32_t expected) {
    PASSTHRU_MSG message{};
    uint32_t count = 9, id = 9;
    CHECK(PassThruReadMsgs(channel, &message, &count, 0) == (expected == ERR_NOT_SUPPORTED ? ERR_BUFFER_EMPTY : expected) && count == 0);
    count = 9;
    // Transmit is implemented now, so a live channel rejects this empty message on its
    // protocol field rather than reporting the whole call unsupported.
    CHECK(PassThruWriteMsgs(channel, &message, &count, 0) ==
          (expected == ERR_NOT_SUPPORTED ? ERR_MSG_PROTOCOL_ID : expected) && count == 0);
    CHECK(PassThruStartPeriodicMsg(channel, &message, &id, 10) == expected && id == 0);
    id = 9;
    CHECK(PassThruStartMsgFilter(channel, BLOCK_FILTER, &message, &message, nullptr, &id) == expected && id == 0);
    CHECK(PassThruStopPeriodicMsg(channel, 1) == expected);
    CHECK(PassThruStopMsgFilter(channel, 1) == (expected == ERR_NOT_SUPPORTED ? ERR_INVALID_FILTER_ID : expected));
}
void lifecycle() {
    auto script = prepare();
    script->connect(); script->disconnect(); script->connect(0, 250000); script->disconnect();
    script->connect(CAN_29BIT_ID); script->disconnect(); script->add(5);
    const auto device = open();
    uint32_t id = 99;
    CHECK(PassThruConnect(device, CAN, 0, 500000, nullptr) == ERR_NULL_PARAMETER);
    CHECK(PassThruConnect(0, CAN, 0, 500000, &id) == ERR_INVALID_DEVICE_ID && id == 0);
    CHECK(PassThruConnect(device, 1234, 0, 500000, &id) == ERR_INVALID_PROTOCOL_ID && id == 0);
    CHECK(PassThruConnect(device, ISO15765, 0, 500000, &id) == ERR_NOT_SUPPORTED && id == 0);
    CHECK(PassThruConnect(device, CAN, 2, 500000, &id) == ERR_INVALID_FLAGS && id == 0);
    CHECK(PassThruConnect(device, CAN, 0, 0, &id) == ERR_INVALID_BAUDRATE && id == 0);
    const auto first = connect(device);
    CHECK(PassThruConnect(device, CAN, 0, 500000, &id) == ERR_CHANNEL_IN_USE && id == 0);
    CHECK(PassThruConnect(device, ISO15765, 0, 500000, &id) == ERR_CHANNEL_IN_USE && id == 0);
    CHECK(PassThruClose(first) == ERR_INVALID_DEVICE_ID);
    CHECK(PassThruDisconnect(device) == ERR_INVALID_CHANNEL_ID);
    unsupported_operations(first, ERR_NOT_SUPPORTED);
    CHECK(PassThruDisconnect(first) == 0);
    CHECK(PassThruDisconnect(first) == ERR_INVALID_CHANNEL_ID);
    unsupported_operations(first, ERR_INVALID_CHANNEL_ID);
    const auto second = connect(device, 0, 250000); CHECK(second > first);
    CHECK(PassThruDisconnect(first) == ERR_INVALID_CHANNEL_ID);
    CHECK(PassThruDisconnect(second) == 0);
    const auto third = connect(device, CAN_29BIT_ID); CHECK(third > second);
    CHECK(PassThruClose(device) == 0); // implicit channel teardown
    CHECK(PassThruDisconnect(third) == ERR_INVALID_CHANNEL_ID);
    CHECK(PassThruConnect(device, CAN, 0, 500000, &id) == ERR_INVALID_DEVICE_ID && id == 0);
    script->finished();
}
void firmware_rejections() {
    auto script = prepare();
    // Nonzero, uncaptured baud must reach firmware, not an invented host whitelist.
    script->add(6, unhex("0000000039300000"), channel_node(CAN), 0x203);
    script->add(6, unhex("0000000020a10700"), channel_node(CAN));
    script->add(0x12, unhex("01000000060000000e000000"), channel_node(CAN), 0x203);
    script->disconnect();
    script->connect(); script->disconnect(); script->add(5);
    const auto device = open(); uint32_t id = 99;
    CHECK(PassThruConnect(device, CAN, 0, 12345, &id) == ERR_FAILED && id == 0);
    char error[80]; PassThruGetLastError(error); CHECK(std::strstr(error, "0x00000203"));
    CHECK(PassThruConnect(device, CAN, 0, 500000, &id) == ERR_FAILED && id == 0);
    PassThruGetLastError(error); CHECK(std::strstr(error, "0x00000203"));
    const auto channel = connect(device); CHECK(PassThruDisconnect(channel) == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void rollback_failure() {
    auto script = prepare();
    script->add(6, unhex("0000000020a10700"), channel_node(CAN));
    script->add(0x12, unhex("01000000060000000e000000"), channel_node(CAN), 0x203);
    script->add(7, {}, channel_node(CAN), 7);
    script->disconnect(); script->add(5); // Close retries cleanup
    const auto device = open(); uint32_t id = 99;
    CHECK(PassThruConnect(device, CAN, 0, 500000, &id) == ERR_FAILED && id == 0);
    char error[80]; PassThruGetLastError(error); CHECK(std::strstr(error, "0x00000203"));
    CHECK(PassThruConnect(device, CAN, 0, 500000, &id) == ERR_DEVICE_NOT_CONNECTED && id == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void teardown_failure(bool implicit) {
    auto script = prepare(); script->connect();
    script->add(7, {}, channel_node(CAN), 0x203);
    if (!implicit) script->disconnect();
    script->add(5);
    const auto device = open(), channel = connect(device);
    if (implicit) CHECK(PassThruClose(device) == ERR_FAILED);
    else {
        CHECK(PassThruDisconnect(channel) == ERR_FAILED);
        unsupported_operations(channel, ERR_INVALID_CHANNEL_ID);
        uint32_t id = 99;
        CHECK(PassThruConnect(device, CAN, 0, 500000, &id) == ERR_DEVICE_NOT_CONNECTED && id == 0);
        CHECK(PassThruClose(device) == 0);
    }
    unsupported_operations(channel, ERR_INVALID_CHANNEL_ID);
    CHECK(PassThruClose(device) == ERR_INVALID_DEVICE_ID); script->finished();
}
void independent_devices() {
    auto first = prepare(); first->connect(); first->disconnect(); first->add(5);
    const auto first_device = open(), first_channel = connect(first_device);
    auto second = prepare(); second->connect(); second->disconnect(); second->add(5);
    const auto second_device = open(), second_channel = connect(second_device);
    CHECK(second_device > first_channel && second_channel > second_device);
    CHECK(PassThruClose(first_device) == 0); first->finished();
    unsupported_operations(second_channel, ERR_NOT_SUPPORTED);
    CHECK(PassThruClose(second_device) == 0); second->finished();
}
void ambiguous_command(Outcome outcome, bool pin, int32_t expected) {
    auto script = prepare();
    script->add(6, unhex("0000000020a10700"), channel_node(CAN));
    if (pin) script->add(0x12, unhex("01000000060000000e000000"), channel_node(CAN));
    script->steps.back().outcome = outcome;
    const auto device = open(); uint32_t id = 99;
    CHECK(PassThruConnect(device, CAN, 0, 500000, &id) == expected && id == 0);
    char error[80]; PassThruGetLastError(error);
    if (outcome == Outcome::Unplug) CHECK(std::strstr(error, "unplugged"));
    CHECK(PassThruConnect(device, CAN, 0, 500000, &id) == ERR_DEVICE_NOT_CONNECTED && id == 0);
    CHECK(PassThruClose(device) == ERR_DEVICE_NOT_CONNECTED);
    script->finished(); // poisoned session must emit no further commands
    auto fresh = prepare(); fresh->connect(); fresh->disconnect(); fresh->add(5);
    const auto reopened = open(); CHECK(reopened > device); connect(reopened);
    CHECK(PassThruClose(reopened) == 0); fresh->finished();
}
void competing_connects() {
    auto script = prepare(); script->connect(); script->disconnect(); script->add(5);
    const auto device = open();
    std::barrier start(3);
    uint32_t first = 99, second = 99;
    auto attempt = [&](uint32_t &id) { start.arrive_and_wait(); return PassThruConnect(device, CAN, 0, 500000, &id); };
    auto a = std::async(std::launch::async, attempt, std::ref(first));
    auto b = std::async(std::launch::async, attempt, std::ref(second));
    start.arrive_and_wait(); const auto ar = a.get(), br = b.get();
    CHECK((ar == 0 && br == ERR_CHANNEL_IN_USE && second == 0) || (br == 0 && ar == ERR_CHANNEL_IN_USE && first == 0));
    CHECK(PassThruClose(device) == 0); script->finished();
}
// Hold the first wire operation while the competing API call starts. Never use
// sleeps to select an interleaving; the wire script rejects interleaved teardown.
void lifecycle_race(bool disconnect_first, bool close_first) {
    auto script = prepare();
    if (!close_first || disconnect_first) { script->connect(); script->disconnect(); }
    script->add(5);
    const auto device = open();
    uint32_t channel = disconnect_first ? connect(device) : 0;
    std::promise<void> entered, release, competitor_started;
    auto released = release.get_future().share();
    script->steps.front().before_reply = [&] { entered.set_value(); released.wait(); };
    auto first = std::async(std::launch::async, [&] {
        if (close_first) return PassThruClose(device);
        if (disconnect_first) return PassThruDisconnect(channel);
        return PassThruConnect(device, CAN, 0, 500000, &channel);
    });
    entered.get_future().wait();
    uint32_t other = 99;
    auto second = std::async(std::launch::async, [&] {
        competitor_started.set_value();
        if (!close_first) return PassThruClose(device);
        if (disconnect_first) return PassThruDisconnect(channel);
        return PassThruConnect(device, CAN, 0, 500000, &other);
    });
    competitor_started.get_future().wait();
    // With setup/teardown held on the wire, Close must not finish or send a
    // command. The bounded wait observes blocking; it does not pick a winner.
    const bool blocked = close_first || second.wait_for(20ms) == std::future_status::timeout;
    release.set_value();
    CHECK(first.get() == 0);
    const auto result = second.get();
    CHECK(result == (close_first ? (disconnect_first ? ERR_INVALID_CHANNEL_ID : ERR_INVALID_DEVICE_ID) : 0));
    if (close_first && !disconnect_first) CHECK(other == 0);
    if (channel) CHECK(PassThruDisconnect(channel) == ERR_INVALID_CHANNEL_ID);
    CHECK(blocked);
    script->finished();
}
std::vector<std::string> fixture_lines(const char *name) {
    std::ifstream file(std::string(FIXTURE_DIR) + "/" + name); CHECK(file.good());
    std::vector<std::string> result;
    for (std::string line; std::getline(file, line); ) if (!line.empty() && line[0] != '#') result.push_back(line);
    return result;
}
PASSTHRU_MSG filter_message(const char *data, uint32_t flags = 0) {
    PASSTHRU_MSG message{}; message.ProtocolID = CAN; message.TxFlags = flags;
    const auto bytes = unhex(data); message.DataSize = static_cast<uint32_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), message.Data); return message;
}
void add_filter_step(Script &script, uint32_t handle, uint16_t status = 0, uint32_t flags = 0) {
    auto payload = unhex("0000000000000104000007ff000007e8"); put16(payload, 4, static_cast<uint16_t>(flags));
    script.add(0x0d, payload, channel_node(CAN), status);
    auto &wire = script.steps.back().exchange.in[0];
    wire.resize(28); put16(wire, 0, 24); put16(wire, 2, 24 ^ 0x51e6);
    put16(wire, 24, static_cast<uint16_t>(handle)); put16(wire, 26, static_cast<uint16_t>(handle >> 16));
}
void remove_filter_step(Script &script, uint32_t handle, uint16_t status = 0) {
    Bytes payload(8, 0); put16(payload, 4, static_cast<uint16_t>(handle)); put16(payload, 6, static_cast<uint16_t>(handle >> 16));
    script.add(0x0e, payload, channel_node(CAN), status);
}
uint32_t add_filter(uint32_t channel, uint32_t flags = 0) {
    auto mask = filter_message("000007ff", flags), pattern = filter_message("000007e8", flags);
    uint32_t id = 99;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &id) == 0);
    CHECK(id > channel); return id;
}
void filter_contract() {
    auto script = prepare(); script->connect();
    // D2 first add and its opaque allocation handle, unchanged except seq/token.
    script->captured("1c00fa51010500000d000a00000000000000000000000104000007ff000007e8",
                     "1800fe51000001050d800a00000000000000000054783d00787e0000");
    add_filter_step(*script, 0xdead82a0); // high bits must survive removal too
    remove_filter_step(*script, 0xdead82a0, 0x203); // definite rejection retains mapping
    remove_filter_step(*script, 0xdead82a0);
    remove_filter_step(*script, 0x7e78);
    add_filter_step(*script, 0); // firmware handles are opaque, including zero
    script->disconnect(); script->connect(); script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    auto mask = filter_message("000007ff"), pattern = filter_message("000007e8"); uint32_t id = 99;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, nullptr, &pattern, nullptr, &id) == ERR_NULL_PARAMETER && id == 0);
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, nullptr) == ERR_NULL_PARAMETER);
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, &pattern, &id) == ERR_INVALID_MSG && id == 0);
    mask.ProtocolID = ISO15765;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &id) == ERR_MSG_PROTOCOL_ID && id == 0);
    mask.ProtocolID = CAN; mask.DataSize = 3;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &id) == ERR_INVALID_MSG && id == 0);
    mask.DataSize = 13; pattern.DataSize = 13;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &id) == ERR_INVALID_MSG && id == 0);
    mask.DataSize = 4; pattern.DataSize = 5;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &id) == ERR_INVALID_MSG && id == 0);
    pattern.DataSize = 4; mask.TxFlags = 2;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &id) == ERR_INVALID_FLAGS && id == 0);
    mask.TxFlags = CAN_29BIT_ID;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &id) == ERR_INVALID_MSG && id == 0);
    const auto first = add_filter(channel), second = add_filter(channel); CHECK(second > first);
    CHECK(PassThruStopMsgFilter(channel, device) == ERR_INVALID_FILTER_ID);
    CHECK(PassThruStopMsgFilter(channel, second) == ERR_FAILED);
    CHECK(PassThruStopMsgFilter(channel, second) == 0);
    CHECK(PassThruStopMsgFilter(channel, second) == ERR_INVALID_FILTER_ID);
    CHECK(PassThruStopMsgFilter(channel, first) == 0);
    const auto third = add_filter(channel); CHECK(third > second);
    CHECK(PassThruDisconnect(channel) == 0); // channel close owns implicit filter cleanup
    const auto replacement = connect(device);
    CHECK(PassThruStopMsgFilter(replacement, third) == ERR_INVALID_FILTER_ID);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void many_filters() {
    auto script = prepare(); script->connect(CAN_29BIT_ID);
    for (uint32_t i = 0; i < 40; ++i) add_filter_step(*script, 1000 + i * 73, 0, CAN_29BIT_ID);
    for (uint32_t i = 40; i-- > 0;) remove_filter_step(*script, 1000 + i * 73);
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device, CAN_29BIT_ID);
    std::vector<uint32_t> ids;
    for (unsigned i = 0; i < 40; ++i) ids.push_back(add_filter(channel, CAN_29BIT_ID));
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) CHECK(PassThruStopMsgFilter(channel, *it) == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void filter_failure(bool missing_handle) {
    auto script = prepare(); script->connect();
    add_filter_step(*script, 0x1234, missing_handle ? 0 : 0x203);
    if (missing_handle) {
        auto &wire = script->steps.back().exchange.in[0]; wire.resize(24); put16(wire, 0, 20); put16(wire, 2, 20 ^ 0x51e6);
    } else { add_filter_step(*script, 0x4321); }
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    auto mask = filter_message("000007ff"), pattern = filter_message("000007e8"); uint32_t id = 99;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &id) == ERR_FAILED && id == 0);
    if (missing_handle) {
        CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &id) == ERR_DEVICE_NOT_CONNECTED && id == 0);
        PASSTHRU_MSG message{}; uint32_t count = 1;
        CHECK(PassThruReadMsgs(channel, &message, &count, 0) == ERR_DEVICE_NOT_CONNECTED && count == 0);
    } else add_filter(channel);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void captured_receive() {
    auto script = prepare(); script->connect();
    // D1 wildcard add, and the associated first 32 live-bus frames.
    script->captured("1c00fa51010500000d000a000000000000000000000001040000000000000000",
                     "1800fe51000001050d800a0000000000000000000c523d00d46e0000");
    Bytes stream;
    for (const auto &line : fixture_lines("can-receive.frames")) {
        const auto wire = unhex(line); stream.insert(stream.end(), wire.begin(), wire.end());
    }
    // Data arriving before the filter acknowledgement must queue, not satisfy
    // the pending command. Fragment every frame through seven-byte chunks.
    auto &chunks = script->steps.back().exchange.in;
    auto response = chunks.back(); chunks.clear();
    for (size_t i = 0; i < stream.size(); i += 7)
        chunks.emplace_back(stream.begin() + static_cast<ptrdiff_t>(i), stream.begin() + static_cast<ptrdiff_t>(std::min(i + 7, stream.size())));
    chunks.push_back(response);
    remove_filter_step(*script, 0x6ed4); script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    auto mask = filter_message("00000000"), pattern = mask; uint32_t filter = 0;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &filter) == 0);
    std::vector<PASSTHRU_MSG> messages(64); uint32_t count = 64;
    CHECK(PassThruReadMsgs(channel, messages.data(), &count, 5) == ERR_TIMEOUT && count == 32);
    const auto expected = fixture_lines("can-receive.expected"); CHECK(expected.size() == count);
    for (size_t i = 0; i < count; ++i) {
        const auto &m = messages[i]; std::ostringstream line;
        line << "message protocol=" << m.ProtocolID << " rx=" << std::hex << m.RxStatus << " tx=" << m.TxFlags << std::dec
             << " timestamp=" << m.Timestamp << " size=" << m.DataSize << " extra=" << m.ExtraDataIndex
             << " data=" << hex(std::span(m.Data, m.DataSize));
        CHECK(line.str() == expected[i]);
    }
    count = 1; CHECK(PassThruReadMsgs(channel, messages.data(), &count, 1) == ERR_BUFFER_EMPTY && count == 0);
    count = 0; CHECK(PassThruReadMsgs(channel, messages.data(), &count, 0) == ERR_FAILED && count == 0);
    count = 10001; CHECK(PassThruReadMsgs(channel, messages.data(), &count, 0) == ERR_FAILED && count == 0);
    count = 1; CHECK(PassThruReadMsgs(channel, nullptr, &count, 0) == ERR_NULL_PARAMETER && count == 0);
    CHECK(PassThruStopMsgFilter(channel, filter) == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
Bytes inbound_frame() { return unhex(fixture_lines("can-receive.frames").front()); }
void receive_edges() {
    auto script = prepare(); script->connect(); script->disconnect(); script->connect(); script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    const auto original = inbound_frame(); auto modified = original;
    // Wrong source/destination and unrelated indication are discarded.
    put16(modified, 6, channel_node(ISO15765)); script->receive(modified);
    modified = original; put16(modified, 4, 1); script->receive(modified);
    modified = original; put16(modified, 8, 10); script->receive(modified);
    PASSTHRU_MSG message{}; uint32_t count = 1;
    CHECK(PassThruReadMsgs(channel, &message, &count, 0) == ERR_BUFFER_EMPTY && count == 0);
    modified = original; put16(modified, 24, 0xffff); // ignored body+20, NOT data length
    put16(modified, 16, 0xffff); put16(modified, 18, 0xffff); // all wire status bits
    put16(modified, 20, 0xffff); put16(modified, 22, 0xffff); // timestamp wrap boundary
    script->receive(modified); count = 1;
    CHECK(PassThruReadMsgs(channel, &message, &count, 0) == 0 && count == 1);
    CHECK(message.DataSize == 12 && message.ExtraDataIndex == 12 && message.RxStatus == 0xc0000100U && message.Timestamp == UINT32_MAX && message.TxFlags == 0);
    modified = original; modified.resize(32);
    put16(modified, 0, 28); put16(modified, 2, 28 ^ 0x51e6); put16(modified, 26, 4);
    script->receive(modified);
    std::array<PASSTHRU_MSG, 2> partial{}; count = 2;
    CHECK(PassThruReadMsgs(channel, partial.data(), &count, 0) == 0 && count == 1 && partial[0].DataSize == 4);
    for (uint16_t size : std::array<uint16_t, 3>{3, 13, 4096}) {
        modified = original; put16(modified, 26, size); script->receive(modified); count = 1;
        CHECK(PassThruReadMsgs(channel, &message, &count, 0) == ERR_INVALID_MSG && count == 0);
    }
    Bytes short_body(12, 0); put16(short_body, 2, channel_node(CAN)); put16(short_body, 4, 9);
    script->receive(encode(short_body)); count = 1;
    CHECK(PassThruReadMsgs(channel, &message, &count, 0) == ERR_INVALID_MSG && count == 0);
    // Keep oldest 4096 messages, discard newest and report overflow once.
    Bytes burst;
    for (unsigned i = 0; i < 4097; ++i) burst.insert(burst.end(), original.begin(), original.end());
    script->receive(burst);
    std::vector<PASSTHRU_MSG> messages(4096); count = 4096;
    CHECK(PassThruReadMsgs(channel, messages.data(), &count, 0) == ERR_BUFFER_OVERFLOW && count == 4096);
    count = 1; CHECK(PassThruReadMsgs(channel, &message, &count, 0) == ERR_BUFFER_EMPTY && count == 0);
    for (uint16_t code : std::array<uint16_t, 2>{0x10b, 0x119}) {
        Bytes body(20, 0); put16(body, 2, channel_node(CAN)); put16(body, 4, 10); put16(body, 12, code);
        script->receive(encode(body)); count = 1;
        CHECK(PassThruReadMsgs(channel, &message, &count, 0) == ERR_BUFFER_OVERFLOW && count == 0);
    }
    script->receive(original); CHECK(PassThruDisconnect(channel) == 0);
    const auto replacement = connect(device); count = 1;
    CHECK(PassThruReadMsgs(replacement, &message, &count, 0) == ERR_BUFFER_EMPTY && count == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void waiting_read(unsigned action) {
    auto script = prepare(); script->connect();
    if (action != 3) { script->disconnect(); script->add(5); }
    const auto device = open(), channel = connect(device);
    std::promise<void> started;
    PASSTHRU_MSG message{}; uint32_t count = 1;
    auto read = std::async(std::launch::async, [&] { started.set_value(); return PassThruReadMsgs(channel, &message, &count, 5000); });
    started.get_future().wait();
    const bool blocked = read.wait_for(20ms) == std::future_status::timeout;
    if (action == 0) script->receive(inbound_frame());
    if (action == 1) CHECK(PassThruDisconnect(channel) == 0);
    if (action == 2) CHECK(PassThruClose(device) == 0);
    if (action == 3) script->fail("test unplug while waiting for CAN");
    CHECK(read.wait_for(1s) == std::future_status::ready);
    const auto result = read.get(); CHECK(blocked);
    CHECK(result == (action == 0 ? 0 : action == 3 ? ERR_DEVICE_NOT_CONNECTED : ERR_INVALID_CHANNEL_ID));
    CHECK(count == (action == 0 ? 1U : 0U));
    if (action != 2) CHECK(PassThruClose(device) == (action == 3 ? ERR_DEVICE_NOT_CONNECTED : 0));
    script->finished();
}

void filter_transport_failure(bool stopping) {
    auto script = prepare(); script->connect(); add_filter_step(*script, 0x12345678);
    if (stopping) remove_filter_step(*script, 0x12345678);
    script->steps.back().outcome = Outcome::ShortWrite;
    const auto device = open(), channel = connect(device);
    if (stopping) {
        const auto filter = add_filter(channel);
        CHECK(PassThruStopMsgFilter(channel, filter) == ERR_FAILED);
        CHECK(PassThruStopMsgFilter(channel, filter) == ERR_DEVICE_NOT_CONNECTED);
    } else {
        auto mask = filter_message("000007ff"), pattern = filter_message("000007e8"); uint32_t filter = 99;
        CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &filter) == ERR_FAILED && filter == 0);
    }
    PASSTHRU_MSG message{}; uint32_t count = 1;
    CHECK(PassThruReadMsgs(channel, &message, &count, 0) == ERR_DEVICE_NOT_CONNECTED && count == 0);
    CHECK(PassThruClose(device) == ERR_DEVICE_NOT_CONNECTED); script->finished();
}
void partial_read_cancel() {
    auto script = prepare(); script->connect(); script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    script->receive(inbound_frame());
    std::array<PASSTHRU_MSG, 2> messages{}; uint32_t count = 2; std::promise<void> started;
    auto read = std::async(std::launch::async, [&] {
        started.set_value(); return PassThruReadMsgs(channel, messages.data(), &count, 5000);
    });
    started.get_future().wait();
    const bool blocked = read.wait_for(20ms) == std::future_status::timeout;
    CHECK(PassThruDisconnect(channel) == 0);
    CHECK(read.get() == ERR_INVALID_CHANNEL_ID); CHECK(blocked && count == 1);
    CHECK(messages[0].DataSize == 12);
    CHECK(PassThruClose(device) == 0); script->finished();
}

PASSTHRU_MSG can_message(const char *data, uint32_t flags = 0) {
    PASSTHRU_MSG message{};
    message.ProtocolID = CAN; message.TxFlags = flags;
    const auto bytes = unhex(data);
    message.DataSize = static_cast<uint32_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), message.Data);
    return message;
}
void transmit() {
    auto script = prepare(); script->connect();
    // The vendor's captured cOutboundData body is
    //   40000000 e8030000 06000000 000007df 0902
    // = TxFlags | timeout | DataSize | ExtraDataIndex | big-endian ID and data, on an
    // ISO15765 channel. A raw CAN frame differs only in the flags word, since
    // ISO15765_FRAME_PAD cannot apply to a CAN channel. chan is 1 for data commands.
    script->add(8, unhex("00000000e803000006000000000007df0902"), channel_node(CAN), 0x100, data_chan);
    // Status zero is equally acceptable; only 0x100 is the documented queued case.
    script->add(8, unhex("00000000640000000a00080000012345010200000000"), channel_node(CAN), 0, data_chan);
    script->add(8, unhex("000000006400000005000000000007e001"), channel_node(CAN), 0x203, data_chan);
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);

    auto message = can_message("000007df0902");
    uint32_t count = 1;
    CHECK(PassThruWriteMsgs(channel, &message, &count, 1000) == 0 && count == 1);

    std::array<PASSTHRU_MSG, 2> pair{can_message("00012345010200000000"), can_message("000007e001")};
    pair[0].ExtraDataIndex = 8;
    count = 2;
    // The second message is refused by firmware, so the count reports the one that was
    // accepted rather than zero or two.
    CHECK(PassThruWriteMsgs(channel, pair.data(), &count, 100) == ERR_FAILED && count == 1);
    char error[80]; PassThruGetLastError(error); CHECK(std::strstr(error, "0x00000203"));

    // Validation happens before anything reaches the wire, so none of these consume a
    // scripted exchange.
    count = 1; auto wrong = can_message("000007df0902"); wrong.ProtocolID = ISO15765;
    CHECK(PassThruWriteMsgs(channel, &wrong, &count, 0) == ERR_MSG_PROTOCOL_ID && count == 0);
    count = 1; auto flagged = can_message("000007df0902", CAN_29BIT_ID);
    CHECK(PassThruWriteMsgs(channel, &flagged, &count, 0) == ERR_INVALID_MSG && count == 0);
    count = 1; auto unsupported_flag = can_message("000007df0902", 0x40);
    CHECK(PassThruWriteMsgs(channel, &unsupported_flag, &count, 0) == ERR_INVALID_FLAGS && count == 0);
    count = 1; auto tiny = can_message("000007");
    CHECK(PassThruWriteMsgs(channel, &tiny, &count, 0) == ERR_INVALID_MSG && count == 0);
    count = 1; auto huge = can_message("000007df010203040506070809");
    CHECK(PassThruWriteMsgs(channel, &huge, &count, 0) == ERR_INVALID_MSG && count == 0);
    count = 0; CHECK(PassThruWriteMsgs(channel, &message, &count, 0) == ERR_FAILED && count == 0);
    count = 1; CHECK(PassThruWriteMsgs(channel, nullptr, &count, 0) == ERR_NULL_PARAMETER && count == 0);
    CHECK(PassThruWriteMsgs(channel, &message, nullptr, 0) == ERR_NULL_PARAMETER);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void transmit_tx_done() {
    auto script = prepare(); script->connect();
    script->add(8, unhex("00000000e803000006000000000007df0902"), channel_node(CAN), 0x100, data_chan);
    // iMsgTxDone (0x106) arrives unsolicited on the CAN node alongside the response, and
    // must not be mistaken for one. Two loss codes on the same opcode stay overflow.
    Bytes indication(20, 0);
    put16(indication, 2, channel_node(CAN)); put16(indication, 4, 10); put16(indication, 12, 0x106);
    script->steps.back().exchange.in.insert(script->steps.back().exchange.in.begin(), encode(indication));
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    auto message = can_message("000007df0902");
    uint32_t count = 1;
    CHECK(PassThruWriteMsgs(channel, &message, &count, 1000) == 0 && count == 1);
    PASSTHRU_MSG received{};
    count = 1;
    CHECK(PassThruReadMsgs(channel, &received, &count, 1) == ERR_BUFFER_EMPTY && count == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
}
namespace mongoose {
std::unique_ptr<Transport> open_transport(const Selector &, Trace) {
    CHECK(next_script); return std::make_unique<ScriptTransport>(std::exchange(next_script, {}));
}
}
int main() {
    try {
        transmit(); transmit_tx_done();
        filter_contract(); many_filters(); filter_failure(false); filter_failure(true);
        captured_receive(); receive_edges(); partial_read_cancel();
        filter_transport_failure(false); filter_transport_failure(true);
        for (unsigned action = 0; action < 4; ++action) waiting_read(action);
        lifecycle(); firmware_rejections(); rollback_failure(); teardown_failure(false); teardown_failure(true);
        ambiguous_command(Outcome::ShortWrite, false, ERR_FAILED);
        ambiguous_command(Outcome::Unplug, true, ERR_DEVICE_NOT_CONNECTED);
        ambiguous_command(Outcome::Timeout, true, ERR_TIMEOUT);
        competing_connects(); independent_devices();
        for (bool disconnect_first : {false, true}) for (bool close_first : {false, true}) lifecycle_race(disconnect_first, close_first);
        std::cout << "capture-derived CAN lifecycle, transmit, failures, handles and concurrency passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
