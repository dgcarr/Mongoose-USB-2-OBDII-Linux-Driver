#include "config.hpp"
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
    // Periodic messages are implemented, so a live channel rejects this empty message on its protocol field.
    CHECK(PassThruStartPeriodicMsg(channel, &message, &id, 10) ==
          (expected == ERR_NOT_SUPPORTED ? ERR_MSG_PROTOCOL_ID : expected) && id == 0);
    id = 9;
    // BLOCK_FILTER is implemented, so a live channel rejects this empty message on its protocol field.
    CHECK(PassThruStartMsgFilter(channel, BLOCK_FILTER, &message, &message, nullptr, &id) ==
          (expected == ERR_NOT_SUPPORTED ? ERR_MSG_PROTOCOL_ID : expected) && id == 0);
    id = 9;
    CHECK(PassThruStartMsgFilter(channel, 0x99, &message, &message, nullptr, &id) == expected && id == 0);
    CHECK(PassThruStopPeriodicMsg(channel, 1) == (expected == ERR_NOT_SUPPORTED ? ERR_INVALID_MSG_ID : expected));
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
    CHECK(PassThruConnect(device, ISO14230, 0, 500000, &id) == ERR_NOT_SUPPORTED && id == 0);
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
void remove_filter_step(Script &script, uint32_t handle, uint16_t status = 0, uint8_t table = 0) {
    Bytes payload(8, 0); payload[0] = table;
    put16(payload, 4, static_cast<uint16_t>(handle)); put16(payload, 6, static_cast<uint16_t>(handle >> 16));
    script.add(0x0e, payload, channel_node(CAN), status);
}
// A BLOCK filter is the pass-filter body with table selector 1 and type byte 2 (vendor builder
// 1000e600). It is removed from table 1, not table 0; the adapter's handle space is per table on the
// wire even though the handle values are opaque to us.
void add_block_filter_step(Script &script, uint32_t handle) {
    auto payload = unhex("0100000000000204000007ff000007e8");
    script.add(0x0d, payload, channel_node(CAN), 0);
    auto &wire = script.steps.back().exchange.in[0];
    wire.resize(28); put16(wire, 0, 24); put16(wire, 2, 24 ^ 0x51e6);
    put16(wire, 24, static_cast<uint16_t>(handle)); put16(wire, 26, static_cast<uint16_t>(handle >> 16));
}
void block_filter() {
    auto script = prepare(); script->connect();
    add_filter_step(*script, 0x0f5c);                 // a pass filter beside it, table 0
    add_block_filter_step(*script, 0x0fc4);           // table 1, type 2
    remove_filter_step(*script, 0x0fc4, 0, 1);        // removal names the table it was added to
    remove_filter_step(*script, 0x0f5c, 0, 0);
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    auto mask = filter_message("000007ff"), pattern = filter_message("000007e8");
    uint32_t pass = 0, block = 0;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &pass) == 0 && pass);
    CHECK(PassThruStartMsgFilter(channel, BLOCK_FILTER, &mask, &pattern, nullptr, &block) == 0 && block && block != pass);
    CHECK(PassThruStopMsgFilter(channel, block) == 0);
    CHECK(PassThruStopMsgFilter(channel, block) == ERR_INVALID_FILTER_ID);
    CHECK(PassThruStopMsgFilter(channel, pass) == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
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
Bytes inbound_frame();
PASSTHRU_MSG can_message(const char *data, uint32_t flags = 0);
Bytes tx_done_indication(uint16_t sequence, uint16_t node = channel_node(CAN));
// CLEAR_RX_BUFFER, CLEAR_TX_BUFFER and CLEAR_MSG_FILTERS on a channel. Wire forms: cIoctl 0x11 with a
// four-byte selector (2 = TX, 3 = RX) and cTableClear 0x10 with the table selector, one per table the
// channel filled. CLEAR_PERIODIC_MSGS has nothing to clear yet and sends nothing.
void clear_buffers() {
    auto script = prepare(); script->connect();
    add_filter_step(*script, 0x0f5c); add_block_filter_step(*script, 0x0fc4);
    script->add(0x11, unhex("03000000"), channel_node(CAN));   // CLEAR_RX_BUFFER
    script->add(0x11, unhex("02000000"), channel_node(CAN));   // CLEAR_TX_BUFFER
    script->add(0x11, unhex("03000000"), channel_node(CAN), 7); // firmware refuses a clear
    script->add(0x10, unhex("00000000"), channel_node(CAN));   // CLEAR_MSG_FILTERS: table 0 ...
    script->add(0x10, unhex("01000000"), channel_node(CAN));   // ... then table 1
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    auto mask = filter_message("000007ff"), pattern = filter_message("000007e8");
    uint32_t pass = 0, block = 0;
    CHECK(PassThruStartMsgFilter(channel, PASS_FILTER, &mask, &pattern, nullptr, &pass) == 0);
    CHECK(PassThruStartMsgFilter(channel, BLOCK_FILTER, &mask, &pattern, nullptr, &block) == 0);
    // Frames already queued are discarded by CLEAR_RX; ones arriving afterwards are kept.
    const auto frame = inbound_frame();
    script->receive(frame); script->receive(frame);
    PASSTHRU_MSG one{}; uint32_t count = 1;
    CHECK(PassThruIoctl(channel, CLEAR_RX_BUFFER, nullptr, nullptr) == 0);
    CHECK(PassThruReadMsgs(channel, &one, &count, 0) == ERR_BUFFER_EMPTY && count == 0);
    script->receive(frame); count = 1;
    CHECK(PassThruReadMsgs(channel, &one, &count, 0) == 0 && count == 1);
    CHECK(PassThruIoctl(channel, CLEAR_TX_BUFFER, nullptr, nullptr) == 0);
    script->receive(frame);
    CHECK(PassThruIoctl(channel, CLEAR_RX_BUFFER, nullptr, nullptr) == ERR_FAILED);  // refused: nothing is flushed
    count = 1; CHECK(PassThruReadMsgs(channel, &one, &count, 0) == 0 && count == 1);
    CHECK(PassThruIoctl(channel, CLEAR_PERIODIC_MSGS, nullptr, nullptr) == 0);      // no wire traffic
    // Both filters go, one table at a time, and their handles are forgotten.
    CHECK(PassThruIoctl(channel, CLEAR_MSG_FILTERS, nullptr, nullptr) == 0);
    CHECK(PassThruStopMsgFilter(channel, pass) == ERR_INVALID_FILTER_ID);
    CHECK(PassThruStopMsgFilter(channel, block) == ERR_INVALID_FILTER_ID);
    CHECK(PassThruIoctl(channel, CLEAR_MSG_FILTERS, nullptr, nullptr) == 0);        // nothing left: no wire traffic
    CHECK(PassThruIoctl(device, CLEAR_RX_BUFFER, nullptr, nullptr) == ERR_INVALID_CHANNEL_ID);
    CHECK(PassThruIoctl(channel + 100, CLEAR_TX_BUFFER, nullptr, nullptr) == ERR_INVALID_CHANNEL_ID);
    CHECK(PassThruClose(device) == 0); script->finished();
}
// GET_CONFIG and SET_CONFIG. Wire forms from the vendor setters and getters (analysis/decompiled/config):
// cGetValue 0x0c with a four-byte selector, answered with the selector at body+20 and the value at
// body+24; cSetValue 0x0b with selector then value. LOOPBACK is host-side and sends nothing.
void get_value_step(Script &script, uint16_t node, uint32_t selector, uint32_t value, uint16_t status = 0) {
    Bytes payload(4, 0); put16(payload, 0, static_cast<uint16_t>(selector));
    script.add(0x0c, payload, node, status);
    auto &wire = script.steps.back().exchange.in[0];
    wire.resize(32); put16(wire, 0, 28); put16(wire, 2, 28 ^ 0x51e6);
    put16(wire, 24, static_cast<uint16_t>(selector)); put16(wire, 26, 0);
    put16(wire, 28, static_cast<uint16_t>(value)); put16(wire, 30, static_cast<uint16_t>(value >> 16));
}
void set_value_step(Script &script, uint16_t node, uint32_t selector, uint32_t value, uint16_t status = 0) {
    Bytes payload(8, 0); put16(payload, 0, static_cast<uint16_t>(selector));
    put16(payload, 4, static_cast<uint16_t>(value)); put16(payload, 6, static_cast<uint16_t>(value >> 16));
    script.add(0x0b, payload, node, status);
}
void config_can() {
    auto script = prepare(); script->connect();
    constexpr auto node = channel_node(CAN);
    get_value_step(*script, node, 0x04, 500000);   // DATA_RATE
    get_value_step(*script, node, 0x14, 80);       // BIT_SAMPLE_POINT
    get_value_step(*script, node, 0x31, 0);        // DT_PULLUP_VALUE
    set_value_step(*script, node, 0x15, 10);       // SYNC_JUMP_WIDTH = 10
    set_value_step(*script, node, 0x14, 72);       // BIT_SAMPLE_POINT = 72
    get_value_step(*script, node, 0x04, 1, 3);     // firmware refuses: status 3
    get_value_step(*script, node, 0x14, 80);       // a reply that echoes the wrong selector is built below
    script->steps.back().exchange.in[0][24] = 0x15;
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    SCONFIG items[3] = {{cfg_data_rate, 0}, {cfg_bit_sample_point, 0}, {cfg_dt_pullup_value, 9}};
    SCONFIG_LIST list{3, items};
    CHECK(PassThruIoctl(channel, GET_CONFIG, &list, nullptr) == 0);
    CHECK(items[0].Value == 500000 && items[1].Value == 80 && items[2].Value == 0);
    // LOOPBACK is host-side: no wire traffic in either direction.
    SCONFIG loop[1] = {{cfg_loopback, 7}}; SCONFIG_LIST loops{1, loop};
    CHECK(PassThruIoctl(channel, GET_CONFIG, &loops, nullptr) == 0 && loop[0].Value == 0);
    loop[0].Value = 1; CHECK(PassThruIoctl(channel, SET_CONFIG, &loops, nullptr) == 0);
    loop[0].Value = 0; CHECK(PassThruIoctl(channel, GET_CONFIG, &loops, nullptr) == 0 && loop[0].Value == 1);
    loop[0].Value = 2; CHECK(PassThruIoctl(channel, SET_CONFIG, &loops, nullptr) == ERR_INVALID_IOCTL_VALUE);
    loop[0].Value = 0; CHECK(PassThruIoctl(channel, GET_CONFIG, &loops, nullptr) == 0 && loop[0].Value == 1);  // unchanged
    // SET sends one cSetValue per entry, in order.
    SCONFIG sets[2] = {{cfg_sync_jump_width, 10}, {cfg_bit_sample_point, 72}}; SCONFIG_LIST set_list{2, sets};
    CHECK(PassThruIoctl(channel, SET_CONFIG, &set_list, nullptr) == 0);
    // Every entry is checked before any is written: the bad second entry stops the valid first one too.
    SCONFIG mixed[2] = {{cfg_sync_jump_width, 10}, {cfg_bit_sample_point, 60}}; SCONFIG_LIST mixed_list{2, mixed};
    CHECK(PassThruIoctl(channel, SET_CONFIG, &mixed_list, nullptr) == ERR_INVALID_IOCTL_VALUE);
    SCONFIG jump[1] = {{cfg_sync_jump_width, 101}}; SCONFIG_LIST jumps{1, jump};
    CHECK(PassThruIoctl(channel, SET_CONFIG, &jumps, nullptr) == ERR_INVALID_IOCTL_VALUE);
    SCONFIG rate[1] = {{cfg_data_rate, 0}}; SCONFIG_LIST rates{1, rate};
    CHECK(PassThruIoctl(channel, SET_CONFIG, &rates, nullptr) == ERR_INVALID_IOCTL_VALUE);
    rate[0].Value = 1000001; CHECK(PassThruIoctl(channel, SET_CONFIG, &rates, nullptr) == ERR_INVALID_IOCTL_VALUE);
    // Readable but not settable here: an electrical setting.
    SCONFIG pull[1] = {{cfg_dt_pullup_value, 1}}; SCONFIG_LIST pulls{1, pull};
    CHECK(PassThruIoctl(channel, SET_CONFIG, &pulls, nullptr) == ERR_NOT_SUPPORTED);
    // Parameters the channel does not have.
    SCONFIG pins[1] = {{cfg_j1962_pins, 0}}; SCONFIG_LIST pin_list{1, pins};
    CHECK(PassThruIoctl(channel, GET_CONFIG, &pin_list, nullptr) == ERR_FAILED);
    SCONFIG stmin[1] = {{cfg_iso15765_stmin, 0}}; SCONFIG_LIST stmins{1, stmin};
    CHECK(PassThruIoctl(channel, GET_CONFIG, &stmins, nullptr) == ERR_NOT_SUPPORTED);   // ISO15765 only
    // Argument shape, in the vendor's order.
    CHECK(PassThruIoctl(channel, GET_CONFIG, nullptr, nullptr) == ERR_NULL_PARAMETER);
    SCONFIG_LIST empty{0, items}; CHECK(PassThruIoctl(channel, GET_CONFIG, &empty, nullptr) == ERR_FAILED);
    SCONFIG_LIST many{51, items}; CHECK(PassThruIoctl(channel, GET_CONFIG, &many, nullptr) == ERR_FAILED);
    SCONFIG_LIST no_items{1, nullptr}; CHECK(PassThruIoctl(channel, GET_CONFIG, &no_items, nullptr) == ERR_NULL_PARAMETER);
    uint32_t spare = 0; CHECK(PassThruIoctl(channel, GET_CONFIG, &list, &spare) == ERR_FAILED);
    CHECK(PassThruIoctl(device, GET_CONFIG, &list, nullptr) == ERR_INVALID_CHANNEL_ID);
    // A firmware refusal is an error; a reply that echoes another selector is not trusted.
    SCONFIG rate_get[1] = {{cfg_data_rate, 0}}; SCONFIG_LIST rate_get_list{1, rate_get};
    CHECK(PassThruIoctl(channel, GET_CONFIG, &rate_get_list, nullptr) == ERR_FAILED);
    SCONFIG sample[1] = {{cfg_bit_sample_point, 0}}; SCONFIG_LIST sample_list{1, sample};
    CHECK(PassThruIoctl(channel, GET_CONFIG, &sample_list, nullptr) == ERR_FAILED);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void config_iso15765() {
    auto script = prepare();
    constexpr auto node = channel_node(ISO15765);
    script->add(6, unhex("0000000020a10700"), node);
    script->add(0x12, unhex("01000000060000000e000000"), node);
    get_value_step(*script, node, 0x1b, 0);        // ISO15765_BS
    get_value_step(*script, node, 0x1c, 0);        // ISO15765_STMIN
    get_value_step(*script, node, 0x1d, 0xffff);   // BS_TX
    get_value_step(*script, node, 0x23, 1000);     // N_AS_MAX
    get_value_step(*script, node, 0x22, 0);        // ISO15765_PAD_VALUE
    get_value_step(*script, node, 0x22, 0);        // DT_ISO15765_PAD_BYTE: the same selector
    set_value_step(*script, node, 0x1b, 2);        // BS = 2
    set_value_step(*script, node, 0x1c, 20);       // STMIN = 20
    set_value_step(*script, node, 0x1d, 0xffff);   // BS_TX = "no value"
    set_value_step(*script, node, 0x28, 2000);     // N_CR_MAX
    script->add(7, {}, node);
    script->add(5);
    const auto device = open(); uint32_t channel = 0;
    CHECK(PassThruConnect(device, ISO15765, 0, 500000, &channel) == 0);
    SCONFIG gets[6] = {{cfg_iso15765_bs, 9}, {cfg_iso15765_stmin, 9}, {cfg_bs_tx, 0}, {cfg_n_as_max, 0},
                       {cfg_iso15765_pad_value, 9}, {cfg_dt_iso15765_pad_byte, 9}};
    SCONFIG_LIST get_list{6, gets};
    CHECK(PassThruIoctl(channel, GET_CONFIG, &get_list, nullptr) == 0);
    CHECK(gets[0].Value == 0 && gets[1].Value == 0 && gets[2].Value == 0xffff && gets[3].Value == 1000);
    CHECK(gets[4].Value == 0 && gets[5].Value == 0);
    SCONFIG sets[4] = {{cfg_iso15765_bs, 2}, {cfg_iso15765_stmin, 20}, {cfg_bs_tx, 0xffff}, {cfg_n_cr_max, 2000}};
    SCONFIG_LIST set_list{4, sets};
    CHECK(PassThruIoctl(channel, SET_CONFIG, &set_list, nullptr) == 0);
    // Vendor ranges: BS and STMIN are one byte, BS_TX also takes 0xFFFF, the N_* timeouts start at 1,
    // and only 80 is valid as the sample point on ISO15765.
    const std::array<std::pair<uint32_t, uint32_t>, 9> bad{{{cfg_iso15765_bs, 256}, {cfg_iso15765_stmin, 256}, {cfg_bs_tx, 256},
        {cfg_stmin_tx, 0x1000}, {cfg_iso15765_wft_max, 256}, {cfg_n_as_max, 0}, {cfg_n_cs_min, 0x10000},
        {cfg_bit_sample_point, 75}, {cfg_iso15765_pad_value, 256}}};
    for (const auto &[parameter, value] : bad) {
        SCONFIG one[1] = {{parameter, value}}; SCONFIG_LIST one_list{1, one};
        CHECK(PassThruIoctl(channel, SET_CONFIG, &one_list, nullptr) == ERR_INVALID_IOCTL_VALUE);
    }
    // Periodic messages are CAN only: the ISO15765 wire form is not known, and nothing is sent.
    auto iso_message = can_message("000007df0201005555555555"); iso_message.ProtocolID = ISO15765;
    uint32_t iso_periodic = 9;
    CHECK(PassThruStartPeriodicMsg(channel, &iso_message, &iso_periodic, 100) == ERR_NOT_SUPPORTED && iso_periodic == 0);
    SCONFIG half[1] = {{cfg_dt_half_duplex, 1}}; SCONFIG_LIST half_list{1, half};
    CHECK(PassThruIoctl(channel, SET_CONFIG, &half_list, nullptr) == ERR_NOT_SUPPORTED);
    const auto disconnected = PassThruDisconnect(channel);
    if (disconnected) { char text[80] = {0}; PassThruGetLastError(text); std::cerr << "disconnect: " << disconnected << ' ' << text << '\n'; }
    CHECK(disconnected == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
// Periodic messages: cTableAddEntry on table 4 (interval, TxFlags, size, ID and data; vendor 1000d220),
// cTableRemoveEntry with selector 4 and the handle (1000d3c0), cTableClear with selector 4 (1000d4d0).
void add_periodic_step(Script &script, uint32_t interval, uint32_t handle, const char *frame = "000007df0201005555555555") {
    Bytes payload{4, 0, 0, 0};
    for (unsigned shift = 0; shift < 32; shift += 8) payload.push_back(static_cast<uint8_t>(interval >> shift));
    for (unsigned i = 0; i < 4; ++i) payload.push_back(0);   // TxFlags
    const auto data = unhex(frame); payload.push_back(static_cast<uint8_t>(data.size()));
    payload.insert(payload.end(), data.begin(), data.end());
    script.add(0x0d, payload, channel_node(CAN));
    auto &wire = script.steps.back().exchange.in[0];
    wire.resize(28); put16(wire, 0, 24); put16(wire, 2, 24 ^ 0x51e6);
    put16(wire, 24, static_cast<uint16_t>(handle)); put16(wire, 26, static_cast<uint16_t>(handle >> 16));
}
void remove_periodic_step(Script &script, uint32_t handle) {
    Bytes payload(8, 0); payload[0] = 4;
    put16(payload, 4, static_cast<uint16_t>(handle)); put16(payload, 6, static_cast<uint16_t>(handle >> 16));
    script.add(0x0e, payload, channel_node(CAN));
}
void periodic_messages() {
    auto script = prepare(); script->connect();
    add_periodic_step(*script, 1000, 0x0abc);
    const uint16_t first_add = script->sequence;
    add_periodic_step(*script, 500, 0x0def);
    // A periodic frame's own confirmation must not satisfy a timed write that was never confirmed.
    script->add(8, unhex("000000001e0000000c000000000007df0201005555555555"), channel_node(CAN), 0x100, data_chan);
    remove_periodic_step(*script, 0x0abc);
    script->add(0x10, unhex("04000000"), channel_node(CAN));         // CLEAR_PERIODIC_MSGS
    add_periodic_step(*script, 65535, 0x0111);
    script->add(0x10, unhex("04000000"), channel_node(CAN));         // teardown clears the table first ...
    script->disconnect(); script->add(5);                             // ... then closes the channel
    const auto device = open(), channel = connect(device);
    auto message = can_message("000007df0201005555555555");
    uint32_t first = 0, second = 0, third = 0;
    CHECK(PassThruStartPeriodicMsg(channel, &message, &first, 1000) == 0 && first);
    CHECK(PassThruStartPeriodicMsg(channel, &message, &second, 500) == 0 && second && second != first);
    script->receive(tx_done_indication(first_add));                   // the periodic frame goes out
    uint32_t count = 1;
    CHECK(PassThruWriteMsgs(channel, &message, &count, 30) == ERR_TIMEOUT && count == 0);
    CHECK(PassThruStopPeriodicMsg(channel, first) == 0);
    CHECK(PassThruStopPeriodicMsg(channel, first) == ERR_INVALID_MSG_ID);   // already gone
    CHECK(PassThruIoctl(channel, CLEAR_PERIODIC_MSGS, nullptr, nullptr) == 0);
    CHECK(PassThruStopPeriodicMsg(channel, second) == ERR_INVALID_MSG_ID);  // the clear removed it
    CHECK(PassThruIoctl(channel, CLEAR_PERIODIC_MSGS, nullptr, nullptr) == 0);  // nothing left: no wire traffic
    CHECK(PassThruStartPeriodicMsg(channel, &message, &third, 65535) == 0);
    // Checked before anything is sent.
    uint32_t none = 9;
    CHECK(PassThruStartPeriodicMsg(channel, &message, &none, 4) == ERR_INVALID_TIME_INTERVAL && none == 0);
    CHECK(PassThruStartPeriodicMsg(channel, &message, &none, 65536) == ERR_INVALID_TIME_INTERVAL);
    auto wrong = message; wrong.ProtocolID = ISO15765;
    CHECK(PassThruStartPeriodicMsg(channel, &wrong, &none, 100) == ERR_MSG_PROTOCOL_ID);
    auto small = can_message("000007"); CHECK(PassThruStartPeriodicMsg(channel, &small, &none, 100) == ERR_INVALID_MSG);
    auto flagged = message; flagged.TxFlags = 2; CHECK(PassThruStartPeriodicMsg(channel, &flagged, &none, 100) == ERR_INVALID_FLAGS);
    CHECK(PassThruStartPeriodicMsg(channel, nullptr, &none, 100) == ERR_NULL_PARAMETER);
    CHECK(PassThruStartPeriodicMsg(channel, &message, nullptr, 100) == ERR_NULL_PARAMETER);
    CHECK(PassThruStartPeriodicMsg(device, &message, &none, 100) == ERR_INVALID_CHANNEL_ID);
    CHECK(PassThruDisconnect(channel) == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void periodic_limit() {
    auto script = prepare(); script->connect();
    for (uint32_t i = 0; i < 10; ++i) add_periodic_step(*script, 100 + i, 0x200 + i);
    script->add(0x10, unhex("04000000"), channel_node(CAN));
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    auto message = can_message("000007df0201005555555555");
    for (uint32_t i = 0; i < 10; ++i) { uint32_t id = 0; CHECK(PassThruStartPeriodicMsg(channel, &message, &id, 100 + i) == 0 && id); }
    uint32_t extra = 9;
    CHECK(PassThruStartPeriodicMsg(channel, &message, &extra, 200) == ERR_EXCEEDED_LIMIT && extra == 0);
    CHECK(PassThruDisconnect(channel) == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
// An ISO15765 exchange end to end through the public API, as on the car: a flow-control filter, a timed
// write of a mode 09 request whose iMsgTxDone echoes the command's sequence, then the reply as three
// frames. The read must return the transmit-done message, the start-of-message indication and the
// reassembled reply, in that order. This is what the sequence hook exists for: the confirmation
// arrives before the command response, so the library has to know the sequence first.
void add_flow_control_step(Script &script, uint32_t handle, const char *response_id = "000007e8", const char *request_id = "000007e0") {
    Bytes payload = unhex("02000000400000000000000000000000000000000000");
    payload.resize(19);
    const auto response = unhex(response_id), request = unhex(request_id);
    std::copy(response.begin(), response.end(), payload.begin() + 9);
    std::copy(request.begin(), request.end(), payload.begin() + 14);
    script.add(0x0d, payload, channel_node(ISO15765));
    auto &wire = script.steps.back().exchange.in[0];
    wire.resize(28); put16(wire, 0, 24); put16(wire, 2, 24 ^ 0x51e6);
    put16(wire, 24, static_cast<uint16_t>(handle)); put16(wire, 26, static_cast<uint16_t>(handle >> 16));
}
Bytes iso_frame(uint32_t timestamp, const char *frame) {
    Bytes body(24, 0);
    put16(body, 2, channel_node(ISO15765)); put16(body, 4, 9);
    put16(body, 16, static_cast<uint16_t>(timestamp)); put16(body, 18, static_cast<uint16_t>(timestamp >> 16));
    const auto data = unhex(frame);
    put16(body, 22, static_cast<uint16_t>(data.size()));
    body.insert(body.end(), data.begin(), data.end());
    return encode(body);
}
PASSTHRU_MSG iso_message(const char *data, uint32_t flags) {
    PASSTHRU_MSG message{}; message.ProtocolID = ISO15765; message.TxFlags = flags;
    const auto bytes = unhex(data); message.DataSize = static_cast<uint32_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), message.Data); return message;
}
void iso15765_exchange() {
    auto script = prepare();
    constexpr auto node = channel_node(ISO15765);
    script->add(6, unhex("0000000020a10700"), node);
    script->add(0x12, unhex("01000000060000000e000000"), node);
    add_flow_control_step(*script, 0x0f5c);
    script->add(8, unhex("40000000e803000006000000000007df0902"), node, 0x100, 1);
    script->steps.back().exchange.in.insert(script->steps.back().exchange.in.begin(),
                                            tx_done_indication(script->sequence, node));
    // A second timed write whose confirmation never comes must not borrow the first one's.
    script->add(8, unhex("40000000e803000006000000000007df0902"), node, 0x100, 1);
    script->add(7, {}, node);
    script->add(5);
    const auto device = open(); uint32_t channel = 0;
    CHECK(PassThruConnect(device, ISO15765, 0, 500000, &channel) == 0);
    auto mask = iso_message("000000ff", ISO15765_FRAME_PAD), pattern = iso_message("000007e8", ISO15765_FRAME_PAD);
    auto flow = iso_message("000007e0", ISO15765_FRAME_PAD);
    uint32_t filter = 0;
    CHECK(PassThruStartMsgFilter(channel, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &filter) == ERR_INVALID_MSG);  // mask must cover 11 bits
    mask = iso_message("0000ffff", ISO15765_FRAME_PAD);                                                                  // the vendor's own mask
    CHECK(PassThruStartMsgFilter(channel, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &filter) == 0 && filter);
    auto request = iso_message("000007df0902", ISO15765_FRAME_PAD);
    uint32_t count = 1;
    CHECK(PassThruWriteMsgs(channel, &request, &count, 1000) == 0 && count == 1);
    script->receive(iso_frame(6037100, "000007e81014490201524544"));
    script->receive(iso_frame(6038000, "000007e82141435445445649"));
    script->receive(iso_frame(6040500, "000007e8224e303030303030"));
    std::array<PASSTHRU_MSG, 8> out{}; count = 8;
    const auto read = PassThruReadMsgs(channel, out.data(), &count, 0);
    CHECK((read == ERR_TIMEOUT || read == 0) && count == 3);
    CHECK(out[0].RxStatus == 9 && out[0].TxFlags == ISO15765_FRAME_PAD && out[0].DataSize == 4 && out[0].ExtraDataIndex == 0);
    CHECK(std::memcmp(out[0].Data, "\0\0\x07\xdf", 4) == 0);
    CHECK(out[1].RxStatus == START_OF_MESSAGE && out[1].DataSize == 4 && std::memcmp(out[1].Data, "\0\0\x07\xe8", 4) == 0);
    CHECK(out[2].RxStatus == 0 && out[2].DataSize == 24 && out[2].Timestamp == 6040500);
    // No confirmation for the second write: it times out with nothing confirmed, and no message appears.
    count = 1; request = iso_message("000007df0902", ISO15765_FRAME_PAD);
    CHECK(PassThruWriteMsgs(channel, &request, &count, 1000) == ERR_TIMEOUT && count == 0);
    count = 8; CHECK(PassThruReadMsgs(channel, out.data(), &count, 0) == ERR_BUFFER_EMPTY && count == 0);
    CHECK(PassThruDisconnect(channel) == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void flow_control_limit() {
    auto script = prepare();
    constexpr auto node = channel_node(ISO15765);
    script->add(6, unhex("0000000020a10700"), node);
    script->add(0x12, unhex("01000000060000000e000000"), node);
    for (uint32_t i = 0; i < 64; ++i) add_flow_control_step(*script, 0x300 + i);
    script->add(7, {}, node);
    script->add(5);
    const auto device = open(); uint32_t channel = 0;
    CHECK(PassThruConnect(device, ISO15765, 0, 500000, &channel) == 0);
    auto mask = iso_message("ffffffff", ISO15765_FRAME_PAD), pattern = iso_message("000007e8", ISO15765_FRAME_PAD);
    auto flow = iso_message("000007e0", ISO15765_FRAME_PAD);
    for (uint32_t i = 0; i < 64; ++i) { uint32_t id = 0; CHECK(PassThruStartMsgFilter(channel, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &id) == 0); }
    uint32_t extra = 9;
    CHECK(PassThruStartMsgFilter(channel, FLOW_CONTROL_FILTER, &mask, &pattern, &flow, &extra) == ERR_EXCEEDED_LIMIT && extra == 0);
    CHECK(PassThruDisconnect(channel) == 0);
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

PASSTHRU_MSG can_message(const char *data, uint32_t flags) {
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
    // A zero timeout is J2534's queue-and-return write, so these need no tx confirmation.
    script->add(8, unhex("000000000000000006000000000007df0902"), channel_node(CAN), 0x100, data_chan);
    // Status zero is equally acceptable; only 0x100 is the documented queued case.
    // The three-message call tags its commands with the messages still to send: 3, 2, 1.
    script->add(8, unhex("00000000000000000a00080000012345010200000000"), channel_node(CAN), 0, 3);
    script->add(8, unhex("000100000000000006000000000007df0902"), channel_node(CAN), 0x100, 2);
    script->add(8, unhex("000000000000000005000000000007e001"), channel_node(CAN), 0x203, 1);
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);

    auto message = can_message("000007df0902");
    uint32_t count = 1;
    CHECK(PassThruWriteMsgs(channel, &message, &count, 0) == 0 && count == 1);

    std::array<PASSTHRU_MSG, 3> messages{can_message("00012345010200000000"),
                                          can_message("000007df0902", CAN_29BIT_ID),
                                          can_message("000007e001")};
    messages[0].ExtraDataIndex = 8;
    count = 3;
    // Third message is refused by firmware, so count reports the two that were accepted.
    // The second message exercises CAN_29BIT_ID on an 11-bit channel.
    CHECK(PassThruWriteMsgs(channel, messages.data(), &count, 0) == ERR_FAILED && count == 2);
    char error[80]; PassThruGetLastError(error); CHECK(std::strstr(error, "0x00000203"));

    // Validation happens before anything reaches the wire, so none of these consume a
    // scripted exchange.
    count = 1; auto wrong = can_message("000007df0902"); wrong.ProtocolID = ISO15765;
    CHECK(PassThruWriteMsgs(channel, &wrong, &count, 0) == ERR_MSG_PROTOCOL_ID && count == 0);
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
Bytes tx_done_indication(uint16_t sequence, uint16_t node) {
    // Captured shape: dst 0, src the channel node, opcode 10, the originating sequence
    // echoed at +6, chan 1, and code 0x106 at +12. Only a confirmation whose sequence matches a write
    // the library recorded counts, so the caller passes the sequence of the write it confirms.
    Bytes body(20, 0);
    put16(body, 2, node); put16(body, 4, 10); put16(body, 6, sequence); put16(body, 8, 1); put16(body, 12, 0x106);
    return encode(body);
}
void transmit_confirmed() {
    auto script = prepare(); script->connect();
    script->add(8, unhex("00000000e803000006000000000007df0902"), channel_node(CAN), 0x100, data_chan);
    // iMsgTxDone arrives on the CAN node alongside the response and must not be mistaken
    // for one. A timed write reports what the adapter confirmed it sent.
    script->steps.back().exchange.in.insert(script->steps.back().exchange.in.begin(), tx_done_indication(script->sequence));
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    auto message = can_message("000007df0902");
    uint32_t count = 1;
    CHECK(PassThruWriteMsgs(channel, &message, &count, 1000) == 0 && count == 1);
    PASSTHRU_MSG received{}; count = 1;
    CHECK(PassThruReadMsgs(channel, &received, &count, 1) == ERR_BUFFER_EMPTY && count == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void transmit_unconfirmed() {
    auto script = prepare(); script->connect();
    // The adapter queues the frame and says so, but never confirms it was sent. That is
    // exactly what a bench run with no bus produces, and a timed write must not call it
    // a success: nothing left the controller.
    script->add(8, unhex("000000001e00000006000000000007df0902"), channel_node(CAN), 0x100, data_chan);
    script->disconnect(); script->add(5);
    const auto device = open(), channel = connect(device);
    auto message = can_message("000007df0902");
    uint32_t count = 1;
    CHECK(PassThruWriteMsgs(channel, &message, &count, 30) == ERR_TIMEOUT && count == 0);
    CHECK(PassThruClose(device) == 0); script->finished();
}
void waiting_write(unsigned action) {
    auto script = prepare(); script->connect();
    // 0x100 queued response for the outbound data; no iMsgTxDone confirmation.
    // A timed write will therefore block until timeout or until woke by Disconnect/Close/Unplug.
    script->add(8, unhex("000000008813000006000000000007df0902"), channel_node(CAN), 0x100, data_chan);
    const uint16_t write_sequence = script->sequence;
    if (action != 3) { script->disconnect(); script->add(5); }
    const auto device = open(), channel = connect(device);
    std::promise<void> started;
    auto message = can_message("000007df0902"); uint32_t count = 1;
    auto write = std::async(std::launch::async, [&] {
        started.set_value();
        return PassThruWriteMsgs(channel, &message, &count, 5000);
    });
    started.get_future().wait();
    const bool blocked = write.wait_for(20ms) == std::future_status::timeout;
    if (action == 0) {
        // Confirmation arrives on the CAN node: write finishes normally
        script->receive(tx_done_indication(write_sequence));
        CHECK(write.wait_for(1s) == std::future_status::ready);
        CHECK(write.get() == 0);
        CHECK(count == 1U);
        CHECK(PassThruDisconnect(channel) == 0);
        CHECK(PassThruClose(device) == 0);
        CHECK(blocked);
        script->finished();
        return;
    }
    if (action == 1) CHECK(PassThruDisconnect(channel) == 0);
    if (action == 2) CHECK(PassThruClose(device) == 0);
    if (action == 3) script->fail("test unplug while waiting for CAN write");
    CHECK(write.wait_for(1s) == std::future_status::ready);
    const auto result = write.get(); CHECK(blocked);
    CHECK(result == (action == 3 ? ERR_DEVICE_NOT_CONNECTED : ERR_INVALID_CHANNEL_ID));
    CHECK(count == 0U);
    if (action != 2) CHECK(PassThruClose(device) == (action == 3 ? ERR_DEVICE_NOT_CONNECTED : 0));
    script->finished();
}
}
namespace mongoose {
std::unique_ptr<Transport> open_transport(const Selector &, Trace) {
    CHECK(next_script); return std::make_unique<ScriptTransport>(std::exchange(next_script, {}));
}
}
int main() {
    try {
        transmit(); transmit_confirmed(); transmit_unconfirmed();
        filter_contract(); block_filter(); clear_buffers(); config_can(); config_iso15765(); iso15765_exchange(); flow_control_limit(); periodic_messages(); periodic_limit(); many_filters(); filter_failure(false); filter_failure(true);
        captured_receive(); receive_edges(); partial_read_cancel();
        filter_transport_failure(false); filter_transport_failure(true);
        for (unsigned action = 0; action < 4; ++action) waiting_read(action);
        for (unsigned action = 0; action < 4; ++action) waiting_write(action);
        lifecycle(); firmware_rejections(); rollback_failure(); teardown_failure(false); teardown_failure(true);
        ambiguous_command(Outcome::ShortWrite, false, ERR_FAILED);
        ambiguous_command(Outcome::Unplug, true, ERR_DEVICE_NOT_CONNECTED);
        ambiguous_command(Outcome::Timeout, true, ERR_TIMEOUT);
        competing_connects(); independent_devices();
        for (bool disconnect_first : {false, true}) for (bool close_first : {false, true}) lifecycle_race(disconnect_first, close_first);
        std::cout << "capture-derived CAN lifecycle, transmit, failures, handles and concurrency passed\n";
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
