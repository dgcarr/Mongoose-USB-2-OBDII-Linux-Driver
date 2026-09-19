#include "mongoose/j2534.h"
#include "config.hpp"
#include "session.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
namespace {
using namespace mongoose;
thread_local std::array<char, 80> last_error{};
std::mutex devices_mutex;
struct Device {
    explicit Device(std::shared_ptr<Session> value) : session(std::move(value)) {}
    std::shared_ptr<Session> session;
    std::mutex lifecycle;
    bool closed = false, reopen_required = false, wire_channel_open = false;
    bool periodic_untracked = false;  // an add may have reached the adapter without a handle we hold
    uint32_t channel = 0;
    uint32_t channel_flags = 0;
    uint16_t protocol = CAN;  // protocol of the open channel: CAN or ISO15765
    std::shared_ptr<CanReceiver> receiver;
    struct Filter { uint32_t handle; uint8_t table; };  // opaque firmware handle, and the wire table it lives in
    std::map<uint32_t, Filter> filters;  // public ID -> filter
    std::map<uint32_t, uint32_t> periodics;  // public periodic message ID -> firmware handle (table 4)
};
std::map<uint32_t, std::shared_ptr<Device>> devices, channels;
uint64_t next_handle = 1;
// Callers hold devices_mutex; IDs are shared by device and channel namespaces.
uint32_t allocate_handle() {
    if (next_handle > std::numeric_limits<uint32_t>::max())
        throw Error(ERR_EXCEEDED_LIMIT, "handle IDs exhausted");
    return static_cast<uint32_t>(next_handle++);
}
template<class F> int32_t guarded(F &&operation) noexcept {
    last_error[0] = '\0';
    try { operation(); return STATUS_NOERROR; }
    catch (const Error &error) {
        std::snprintf(last_error.data(), last_error.size(), "%s", error.what()); return error.code;
    } catch (const std::exception &error) {
        std::snprintf(last_error.data(), last_error.size(), "%s", error.what()); return ERR_FAILED;
    } catch (...) {
        std::snprintf(last_error.data(), last_error.size(), "unknown internal failure"); return ERR_FAILED;
    }
}
void required(const void *pointer) { if (!pointer) throw Error(ERR_NULL_PARAMETER, "required pointer is null"); }
struct LockedDevice {
    std::shared_ptr<Device> state;
    std::unique_lock<std::mutex> lock;
    LockedDevice(std::shared_ptr<Device> value, uint32_t channel_id = 0)
        : state(std::move(value)), lock(state->lifecycle) {
        if (state->closed || (channel_id && state->channel != channel_id))
            throw Error(channel_id ? ERR_INVALID_CHANNEL_ID : ERR_INVALID_DEVICE_ID, "invalid or closed handle");
    }
    Session &session() const {
        if (state->reopen_required)
            throw Error(ERR_DEVICE_NOT_CONNECTED, "channel state uncertain; close and reopen device");
        return *state->session;
    }
};
LockedDevice lookup(uint32_t id, bool channel = false) {
    std::shared_ptr<Device> state;
    {
        // Never wait for a lifecycle lock while holding the registry lock.
        std::lock_guard lock(devices_mutex);
        auto &registry = channel ? channels : devices;
        auto found = registry.find(id);
        if (found == registry.end())
            throw Error(channel ? ERR_INVALID_CHANNEL_ID : ERR_INVALID_DEVICE_ID,
                        channel ? "invalid channel ID" : "invalid device ID");
        state = found->second;
    }
    return LockedDevice(std::move(state), channel ? id : 0);
}
LockedDevice device(uint32_t id) { return lookup(id); }
[[noreturn]] void unsupported(const char *operation) {
    throw Error(ERR_NOT_SUPPORTED, std::string(operation) + ": awaiting protocol/hardware validation");
}
// Returns whether the channel had periodic messages running, which the caller must clear on the wire
// before the channel is closed: a message left in the adapter's table would keep transmitting.
bool retire_channel(Device &owner) {
    const bool had_periodic = owner.periodic_untracked || !owner.periodics.empty();
    owner.session->set_can_receiver({});
    owner.receiver.reset();
    owner.filters.clear();
    owner.periodics.clear();
    std::lock_guard lock(devices_mutex);
    channels.erase(owner.channel);
    owner.channel = 0;
    // owner.protocol stays as it was: the CloseChannel that follows must go to the node the channel was
    // opened on (0x0601 for ISO15765, as the vendor sends it), and channel_command derives that node
    // from it. Resetting it here sent an ISO15765 close to the CAN node. The next Connect sets it again.
    return had_periodic;
}
void uncertain_channel(Device &owner) {
    owner.reopen_required = true;
    if (owner.receiver) owner.receiver->stop(ERR_DEVICE_NOT_CONNECTED, "channel state uncertain; reopen device");
}
// Preserve Session's distinction between failures before a write and ambiguous
// wire outcomes. Definite firmware rejection is handled separately by accepted().
Bytes channel_command(Device &owner, uint16_t opcode, std::span<const uint8_t> payload = {},
                      std::chrono::milliseconds timeout = std::chrono::seconds(10), uint16_t chan = 0,
                      const std::function<void(uint16_t)> &sequence_known = {}) {
    try {
        return owner.session->command(opcode, payload, timeout, channel_node(owner.protocol), chan, sequence_known);
    } catch (...) {
        if (!owner.session->usable()) uncertain_channel(owner);
        throw;
    }
}
// Status zero is the general success. Two opcodes answer differently and neither is an
// error: cJumpToFirmware reports 7 when the board is already running firmware, and
// cOutboundData reports 0x100 alongside a successfully queued transmit
// (docs/WINDOWS-FINDINGS.md section D). Both are named rather than folded into a
// widened success test, so an unexpected status from any other command still fails.
enum class Allow { None, AlreadyStarted, Queued };
void accepted(std::span<const uint8_t> response, Allow allow = Allow::None) {
    const uint32_t status = Session::status(response);
    if (status == 0) return;
    if (allow == Allow::AlreadyStarted && status == 7) return;
    if (allow == Allow::Queued && status == 0x100) return;
    if (status == 0x20a) throw Error(ERR_FAILED, "adapter reports missing vehicle-connector voltage (0x020a)");
    char message[80];
    std::snprintf(message, sizeof(message), "adapter status 0x%08x (mapping not yet validated)", status);
    throw Error(ERR_FAILED, message);
}
// Empties the adapter's periodic table (cTableClear on table 4) and forgets what was tracked of it.
void clear_periodic_table(Device &owner) {
    accepted(channel_command(owner, 0x10, Bytes{table_periodic, 0, 0, 0}));
    owner.periodics.clear();
    owner.periodic_untracked = false;
}
// now + timeout, saturating rather than overflowing for a very long timeout.
std::chrono::steady_clock::time_point deadline_after(uint32_t timeout_ms) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto available = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
    const std::chrono::milliseconds duration(timeout_ms);
    return duration >= available ? Clock::time_point::max() : now + duration;
}
// How long to wait for the adapter to acknowledge one queued message: what is left of a timed write's deadline,
// kept within 1..10 s, or one second for a write that only queues.
std::chrono::milliseconds acknowledgement_budget(uint32_t timeout_ms, std::chrono::steady_clock::duration remaining) {
    using std::chrono::milliseconds;
    if (!timeout_ms) return milliseconds(1000);
    return std::clamp(std::chrono::duration_cast<milliseconds>(remaining), milliseconds(1000), milliseconds(10000));
}
// Every message of a write, encoded up front so a bad one is found before anything has been sent.
std::vector<Bytes> encode_write_batch(uint16_t protocol, std::span<const PASSTHRU_MSG> messages, uint32_t timeout_ms) {
    std::vector<Bytes> payloads;
    payloads.reserve(messages.size());
    for (const auto &message : messages)
        payloads.push_back(protocol == ISO15765 ? isotp_transmit(message, timeout_ms) : can_transmit(message, timeout_ms));
    return payloads;
}
uint32_t get_value(Session &session, uint8_t selector) {
    const Bytes payload{selector, 0, 0, 0};
    const auto response = session.command(0xc, payload);
    accepted(response);
    if (response.size() < 28 || le32(response, 20) != selector)
        throw Error(ERR_FAILED, "invalid get-value response");
    return le32(response, 24);
}
}
extern "C" {
int32_t J2534_CALL PassThruOpen(void *name, uint32_t *id) {
    return guarded([&] {
        required(id); *id = 0;
        Selector selector;  // defaults to the cdc_acm backend and any single adapter
        if (name) {
            const char *text = static_cast<const char *>(name);
            const size_t size = strnlen(text, 256);
            if (size == 256) throw Error(ERR_FAILED, "Open name is not NUL-terminated");
            selector = parse_selector(std::string_view(text, size));
        }
        auto session = std::make_shared<Session>(open_transport(selector));
        accepted(session->command(0x103), Allow::AlreadyStarted);
        accepted(session->command(3));
        try {
            accepted(session->command(0x109));
            std::lock_guard lock(devices_mutex);
            const auto allocated = allocate_handle();
            devices.emplace(allocated, std::make_shared<Device>(session)); *id = allocated;
        } catch (...) {
            try { session->command(5); } catch (...) {}
            throw;
        }
    });
}
int32_t J2534_CALL PassThruClose(uint32_t id) {
    return guarded([&] {
        auto owner = device(id);
        auto &state = *owner.state;
        state.closed = true;
        {
            std::lock_guard lock(devices_mutex);
            devices.erase(id);
        }
        const bool periodic = retire_channel(state);
        std::exception_ptr error;
        if (state.wire_channel_open && periodic) {
            try { clear_periodic_table(state); }
            catch (...) { error = std::current_exception(); }
        }
        if (state.wire_channel_open) {
            try { accepted(channel_command(state, 7)); state.wire_channel_open = false; }
            catch (...) { error = std::current_exception(); }
        }
        try { accepted(state.session->command(5)); } catch (...) { if (!error) error = std::current_exception(); }
        try { state.session->close(); } catch (...) { if (!error) error = std::current_exception(); }
        if (error) std::rethrow_exception(error);
    });
}
int32_t J2534_CALL PassThruConnect(uint32_t id, uint32_t protocol, uint32_t flags, uint32_t baud, uint32_t *channel) {
    return guarded([&] {
        required(channel); *channel = 0;
        auto owner = device(id);
        auto &state = *owner.state;
        if (protocol < J1850VPW || protocol > ISO15765) throw Error(ERR_INVALID_PROTOCOL_ID, "unknown protocol ID");
        if (!baud) throw Error(ERR_INVALID_BAUDRATE, "baud rate is zero");
        if (state.reopen_required) throw Error(ERR_DEVICE_NOT_CONNECTED, "channel state uncertain; close and reopen device");
        if ((protocol == CAN || protocol == ISO15765) && state.channel)
            throw Error(ERR_CHANNEL_IN_USE, "CAN hardware is already in use");
        if (protocol != CAN && protocol != ISO15765) unsupported("channel protocol");
        if (!can_baud_supported(baud)) throw Error(ERR_INVALID_BAUDRATE, "unsupported CAN bit rate");
        // 29-bit ISO15765 has no capture, so its filter and addressing layout is unknown.
        const uint32_t allowed = protocol == CAN ? static_cast<uint32_t>(CAN_29BIT_ID) : 0u;
        if (flags & ~allowed) throw Error(ERR_INVALID_FLAGS, protocol == CAN ? "unsupported CAN flags"
                                                                              : "unsupported ISO15765 flags");
        uint32_t allocated;
        {
            std::lock_guard lock(devices_mutex);
            allocated = allocate_handle();
        }
        Bytes payload;
        for (uint32_t word : {flags, baud})
            for (unsigned shift = 0; shift < 32; shift += 8)
                payload.push_back(static_cast<uint8_t>(word >> shift));
        auto receiver = std::make_shared<CanReceiver>(static_cast<uint16_t>(protocol));
        state.protocol = static_cast<uint16_t>(protocol);
        accepted(channel_command(state, 6, payload));
        state.wire_channel_open = true;
        try {
            state.session->set_can_receiver(receiver);
            accepted(channel_command(state, 0x12, Bytes{1,0,0,0,6,0,0,0,14,0,0,0}));
            std::lock_guard lock(devices_mutex);
            channels.emplace(allocated, owner.state);
            state.channel = allocated;
            state.channel_flags = flags;
            state.receiver = std::move(receiver);
            *channel = allocated;
        } catch (...) {
            const auto original = std::current_exception();
            state.session->set_can_receiver({});
            if (!state.reopen_required) {
                try { accepted(channel_command(state, 7)); state.wire_channel_open = false; }
                catch (...) { state.reopen_required = true; }
            }
            std::rethrow_exception(original);
        }
    });
}
int32_t J2534_CALL PassThruDisconnect(uint32_t id) {
    return guarded([&] {
        auto owner = lookup(id, true);
        auto &state = *owner.state;
        const bool periodic = retire_channel(state);
        std::exception_ptr error;
        if (periodic) {
            try { clear_periodic_table(state); }
            catch (...) { error = std::current_exception(); }
        }
        try { accepted(channel_command(state, 7)); state.wire_channel_open = false; }
        catch (...) { state.reopen_required = true; throw; }
        if (error) { state.reopen_required = true; std::rethrow_exception(error); }
    });
}
int32_t J2534_CALL PassThruReadMsgs(uint32_t channel, PASSTHRU_MSG *messages, uint32_t *count, uint32_t timeout) {
    return guarded([&] {
        required(count); const auto requested = *count; *count = 0; required(messages);
        auto owner = lookup(channel, true);
        if (!requested || requested > 10000) throw Error(ERR_FAILED, "requested message count must be 1..10000");
        owner.session(); // reject uncertain state before releasing lifecycle ownership
        auto receiver = owner.state->receiver;
        owner.lock.unlock(); // Disconnect/Close must be able to wake a blocked reader
        receiver->read(messages, requested, *count, timeout);
    });
}
int32_t J2534_CALL PassThruWriteMsgs(uint32_t channel, PASSTHRU_MSG *messages, uint32_t *count, uint32_t timeout) {
    return guarded([&] {
        required(count); const auto requested = *count; *count = 0; required(messages);
        auto owner = lookup(channel, true);
        owner.session();
        auto &state = *owner.state;
        if (!requested || requested > 10000) throw Error(ERR_FAILED, "message count must be 1..10000");
        // Without a flow-control filter the adapter cannot answer a multi-frame reply.
        if (state.protocol == ISO15765 && state.filters.empty())
            throw Error(ERR_NO_FLOW_CONTROL, "ISO15765 channel has no flow-control filter");
        const auto deadline = deadline_after(timeout);
        const auto payloads = encode_write_batch(state.protocol, std::span(messages, requested), timeout);
        auto receiver = state.receiver;
        if (!receiver) throw Error(ERR_DEVICE_NOT_CONNECTED, "channel has no receiver");
        const auto call = std::make_shared<CanReceiver::TransmitCount>();
        try {
            for (uint32_t index = 0; index < requested; ++index) {
                const auto before_send = std::chrono::steady_clock::now();
                if (timeout && before_send >= deadline) throw Error(ERR_TIMEOUT, "write deadline expired");
                const auto budget = acknowledgement_budget(timeout, deadline - before_send);
                // Record the request under its sequence number before it goes out: the adapter's
                // iMsgTxDone echoes that sequence and can reach the reader thread before this thread sees
                // the command response.
                bool noted = false;
                const auto record = [&](uint16_t sequence) {
                    receiver->note_transmit(messages[index], sequence, call); noted = true;
                };
                // The data command's chan field is 1: each message is sent as a transaction of its own. The vendor
                // (1000c270) instead sends a whole call as one transaction, every message counting down the messages
                // still to send, and the firmware answers once, after the last. Waiting for a response per command
                // as this loop does, a count above 1 is never answered: on the adapter a three-message write timed
                // out on its first command, while three one-message writes back to back were all accepted.
                constexpr uint16_t one_message_transaction = 1;
                try {
                    const auto response = channel_command(state, 8, payloads[index], budget, one_message_transaction, record);
                    if (Session::status(response) == 0x101) throw Error(ERR_BUFFER_FULL, "adapter transmit buffer full");
                    accepted(response, Allow::Queued);
                }
                catch (...) { if (noted) receiver->forget_transmit(); throw; }
                if (!timeout) *count = index + 1;  // queue-and-return: accepted is all we claim
            }
            if (!timeout) return;
            // J2534 blocks a timed write until the messages are sent, and the adapter says
            // when that happened: one iMsgTxDone per transmitted frame. Reporting the queued
            // count here instead would claim delivery for frames that never left the
            // controller -- which is exactly what happens with no bus attached.
            owner.lock.unlock(); // Disconnect/Close must be able to wake a blocked writer
            const bool confirmed = receiver->await_transmitted(call, requested, deadline);
            if (!confirmed)
                throw Error(ERR_TIMEOUT, "adapter did not confirm transmission of every message");
        } catch (...) {
            if (timeout) *count = static_cast<uint32_t>(receiver->confirmed(call));
            throw;
        }
        *count = static_cast<uint32_t>(receiver->confirmed(call));
    });
}
// Periodic messages are cTableAddEntry (0x0d) on table 4, removed with cTableRemoveEntry (0x0e) and
// cleared with cTableClear (0x10), all with selector 4 (vendor senders 1000d220, 1000d3c0, 1000d4d0). The
// add is answered with the firmware handle at response+20, as for filters. CAN only: the ISO15765 form
// is not known. Ten at a time, which is what J2534 asks for; the adapter's own limit is unmeasured.
int32_t J2534_CALL PassThruStartPeriodicMsg(uint32_t channel, PASSTHRU_MSG *message, uint32_t *id, uint32_t interval) {
    return guarded([&] {
        required(id); *id = 0; required(message);
        auto owner = lookup(channel, true);
        owner.session();
        auto &state = *owner.state;
        if (state.protocol != CAN) unsupported("periodic messages are implemented for CAN channels only");
        const auto payload = can_periodic(*message, interval);
        if (state.periodics.size() >= 10) throw Error(ERR_EXCEEDED_LIMIT, "at most 10 periodic messages per channel");
        uint32_t allocated;
        { std::lock_guard lock(devices_mutex); allocated = allocate_handle(); }
        const bool was_untracked = state.periodic_untracked;
        state.periodic_untracked = true;  // until the entry is known to sit in `periodics`
        auto response = channel_command(state, 0x0d, payload);
        try { accepted(response); }
        catch (...) { state.periodic_untracked = was_untracked; throw; }
        if (response.size() < 24) {
            uncertain_channel(state);
            throw Error(ERR_FAILED, "periodic response missing firmware handle; reopen device");
        }
        const auto handle = le32(response, 20);
        try { state.periodics.emplace(allocated, handle); }
        catch (...) {
            const auto original = std::current_exception();
            Bytes remove(8, 0); remove[0] = table_periodic;
            put16(remove, 4, static_cast<uint16_t>(handle)); put16(remove, 6, static_cast<uint16_t>(handle >> 16));
            try {
                accepted(channel_command(state, 0x0e, remove));
                state.periodic_untracked = was_untracked;
            } catch (...) { uncertain_channel(state); }
            std::rethrow_exception(original);
        }
        state.periodic_untracked = was_untracked;
        *id = allocated;
    });
}
int32_t J2534_CALL PassThruStopPeriodicMsg(uint32_t channel, uint32_t id) {
    return guarded([&] {
        auto owner = lookup(channel, true);
        owner.session();
        auto &state = *owner.state;
        const auto found = state.periodics.find(id);
        if (found == state.periodics.end()) throw Error(ERR_INVALID_MSG_ID, "invalid periodic message ID for channel");
        Bytes payload(8, 0); payload[0] = table_periodic;
        put16(payload, 4, static_cast<uint16_t>(found->second)); put16(payload, 6, static_cast<uint16_t>(found->second >> 16));
        accepted(channel_command(state, 0x0e, payload));
        state.periodics.erase(found);
    });
}
int32_t J2534_CALL PassThruStartMsgFilter(uint32_t channel, uint32_t type, PASSTHRU_MSG *mask, PASSTHRU_MSG *pattern, PASSTHRU_MSG *flow, uint32_t *id) {
    return guarded([&] {
        required(id); *id = 0; required(mask); required(pattern);
        auto owner = lookup(channel, true);
        owner.session();
        auto &state = *owner.state;
        Bytes payload;
        if (state.protocol == ISO15765) {
            if (type != FLOW_CONTROL_FILTER) unsupported("only ISO15765 FLOW_CONTROL_FILTER is implemented");
            if (!flow) throw Error(ERR_NULL_PARAMETER, "FLOW_CONTROL_FILTER requires a flow-control message");
            // The vendor refuses a 65th flow-control filter ("Only 64 filters are permitted total").
            if (std::count_if(state.filters.begin(), state.filters.end(),
                              [](const auto &entry) { return entry.second.table == table_flow_control; }) >= 64)
                throw Error(ERR_EXCEEDED_LIMIT, "at most 64 flow-control filters per channel");
            payload = isotp_flow_control_filter(*mask, *pattern, *flow);
        } else {
            if (type != PASS_FILTER && type != BLOCK_FILTER)
                unsupported("only CAN PASS_FILTER and BLOCK_FILTER are implemented");
            if (flow) throw Error(ERR_INVALID_MSG, "PASS_FILTER and BLOCK_FILTER require no flow-control message");
            payload = type == PASS_FILTER ? can_pass_filter(*mask, *pattern, state.channel_flags)
                                          : can_block_filter(*mask, *pattern, state.channel_flags);
        }
        const uint8_t table = state.protocol == ISO15765 ? table_flow_control
                            : type == BLOCK_FILTER ? table_block : table_pass;
        uint32_t allocated;
        { std::lock_guard lock(devices_mutex); allocated = allocate_handle(); }
        auto response = channel_command(state, 0x0d, payload);
        accepted(response);
        if (response.size() < 24) {
            uncertain_channel(state);
            throw Error(ERR_FAILED, "filter response missing firmware handle; reopen device");
        }
        const auto handle = le32(response, 20);
        try { state.filters.emplace(allocated, Device::Filter{handle, table}); }
        catch (...) {
            const auto original = std::current_exception();
            Bytes remove(8, 0); remove[0] = table; put16(remove, 4, static_cast<uint16_t>(handle)); put16(remove, 6, static_cast<uint16_t>(handle >> 16));
            try { accepted(channel_command(state, 0x0e, remove)); }
            catch (...) { uncertain_channel(state); }
            std::rethrow_exception(original);
        }
        *id = allocated;
    });
}
int32_t J2534_CALL PassThruStopMsgFilter(uint32_t channel, uint32_t id) {
    return guarded([&] {
        auto owner = lookup(channel, true); owner.session();
        auto &state = *owner.state;
        const auto found = state.filters.find(id);
        if (found == state.filters.end()) throw Error(ERR_INVALID_FILTER_ID, "invalid filter ID for channel");
        Bytes payload(8, 0);
        payload[0] = found->second.table;  // the table the filter was added to
        put16(payload, 4, static_cast<uint16_t>(found->second.handle));
        put16(payload, 6, static_cast<uint16_t>(found->second.handle >> 16));
        accepted(channel_command(state, 0x0e, payload));
        state.filters.erase(found);
    });
}
int32_t J2534_CALL PassThruSetProgrammingVoltage(uint32_t id, uint32_t, uint32_t) {
    return guarded([&] { device(id); unsupported("programming voltage"); });
}
int32_t J2534_CALL PassThruReadVersion(uint32_t id, char *firmware, char *dll, char *api) {
    return guarded([&] {
        required(firmware); required(dll); required(api);
        firmware[0] = dll[0] = api[0] = '\0';
        auto session = device(id);
        const uint32_t version = get_value(session.session(), 0x2b);
        std::snprintf(firmware, 80, "%u.%u.%u.%u", version >> 24,
                      (version >> 16) & 255, (version >> 8) & 255, version & 255);
        std::snprintf(dll, 80, "0.1.0-experimental");
        std::snprintf(api, 80, "04.04");
    });
}
int32_t J2534_CALL PassThruGetLastError(char *description) {
    if (!description) return ERR_NULL_PARAMETER;
    std::snprintf(description, last_error.size(), "%s", last_error.data()); return STATUS_NOERROR;
}
// The four buffer and table IOCTLs act on a channel. Wire forms: cIoctl (0x11) with a four-byte
// selector, 2 = clear TX and 3 = clear RX (vendor senders 1000daf0 and 1000dbf0, both waiting for the
// response); cTableClear (0x10) with the four-byte table selector (verified on the bench for table 0).
void channel_ioctl(uint32_t channel, uint32_t ioctl_id) {
    auto owner = lookup(channel, true);
    owner.session();
    auto &state = *owner.state;
    const auto receiver = state.receiver;
    switch (ioctl_id) {
    case CLEAR_TX_BUFFER:
        accepted(channel_command(state, 0x11, Bytes{2, 0, 0, 0}));
        if (receiver) receiver->forget_transmits();
        return;
    case CLEAR_RX_BUFFER:
        accepted(channel_command(state, 0x11, Bytes{3, 0, 0, 0}));
        // Frames that arrived before the response are stale; anything after it is new.
        if (receiver) receiver->flush_receive();
        return;
    case CLEAR_MSG_FILTERS: {
        // Clear each wire table this channel actually filled, then forget the handles. With no
        // filters there is nothing to say to the adapter.
        for (const uint8_t table : {table_pass, table_block, table_flow_control}) {
            const bool used = std::any_of(state.filters.begin(), state.filters.end(),
                                          [table](const auto &entry) { return entry.second.table == table; });
            if (!used) continue;
            accepted(channel_command(state, 0x10, Bytes{table, 0, 0, 0}));
            for (auto entry = state.filters.begin(); entry != state.filters.end();)
                entry = entry->second.table == table ? state.filters.erase(entry) : std::next(entry);
        }
        return;
    }
    case CLEAR_PERIODIC_MSGS:
        if (!state.periodic_untracked && state.periodics.empty()) return;  // nothing to say to the adapter
        clear_periodic_table(state);
        return;
    default:
        throw Error(ERR_INVALID_IOCTL_ID, "not a channel IOCTL");
    }
}
// GET_CONFIG and SET_CONFIG. The channel is checked first, then the argument shape, in the vendor's
// order (PassThruIoctl case 1 and 2): input and its list pointer must be non-null, 1..50 parameters, and
// the output pointer must be NULL. A get reads each value from the firmware with cGetValue (0x0c) on the
// channel node, the selector at +12 of the body and the value at response+24; a set writes it with
// cSetValue (0x0b), selector then value. LOOPBACK never leaves the host. Unlike the vendor, which
// applies a list one parameter at a time and stops at the first failure, a set validates every entry
// before writing any, so a bad value cannot leave the channel half configured.
void config_ioctl(uint32_t channel, uint32_t ioctl_id, void *input, void *output) {
    auto owner = lookup(channel, true);
    owner.session();
    required(input);
    auto *list = static_cast<SCONFIG_LIST *>(input);
    required(list->ConfigPtr);
    if (list->NumOfParams == 0 || list->NumOfParams > 50)
        throw Error(ERR_FAILED, "NumOfParams is unreasonable; is the input a bogus pointer?");
    if (output) throw Error(ERR_FAILED, "the output parameter must be NULL");
    auto &state = *owner.state;
    const auto receiver = state.receiver;
    const std::span<SCONFIG> items(list->ConfigPtr, list->NumOfParams);
    if (ioctl_id == SET_CONFIG)
        for (const auto &item : items) config_validate_set(state.protocol, item.Parameter, item.Value);
    for (auto &item : items) {
        const auto route = config_route(state.protocol, item.Parameter);
        if (route.where == ConfigRoute::Where::HostLoopback) {
            if (!receiver) throw Error(ERR_DEVICE_NOT_CONNECTED, "channel has no receiver");
            if (ioctl_id == GET_CONFIG) item.Value = receiver->loopback() ? 1 : 0;
            else receiver->set_loopback(item.Value != 0);
            continue;
        }
        if (ioctl_id == GET_CONFIG) {
            const auto response = channel_command(state, 0xc, Bytes{static_cast<uint8_t>(route.selector), 0, 0, 0});
            accepted(response);
            if (response.size() < 28 || le32(response, 20) != route.selector)
                throw Error(ERR_FAILED, "invalid get-value response");
            item.Value = le32(response, 24);
        } else {
            Bytes payload(8, 0);
            put16(payload, 0, static_cast<uint16_t>(route.selector));
            put16(payload, 4, static_cast<uint16_t>(item.Value)); put16(payload, 6, static_cast<uint16_t>(item.Value >> 16));
            accepted(channel_command(state, 0xb, payload));
        }
    }
}
int32_t J2534_CALL PassThruIoctl(uint32_t id, uint32_t ioctl_id, void *input, void *output) {
    return guarded([&] {
        if (ioctl_id == GET_CONFIG || ioctl_id == SET_CONFIG) {
            config_ioctl(id, ioctl_id, input, output);
            return;
        }
        if (ioctl_id == CLEAR_TX_BUFFER || ioctl_id == CLEAR_RX_BUFFER || ioctl_id == CLEAR_MSG_FILTERS ||
            ioctl_id == CLEAR_PERIODIC_MSGS) {
            channel_ioctl(id, ioctl_id);
            return;
        }
        // The vendor answers an IoctlID it does not recognise, and one that does not apply to the
        // protocol (FIVE_BAUD_INIT or FAST_INIT on CAN, the functional-address table off ISO9141/14230),
        // with ERR_INVALID_IOCTL_ID (code 0xf), before it looks at the handle.
        if (ioctl_id != READ_VBATT && ioctl_id != READ_PROG_VOLTAGE)
            throw Error(ERR_INVALID_IOCTL_ID, "IoctlID unrecognized, or not valid for this protocol");
        auto session = device(id);
        required(output);
        const uint32_t raw = get_value(session.session(), ioctl_id == READ_VBATT ? 3 : 2);
        // Vendor 10059d60 rounds millivolts to the nearest 100, ties upward.
        const uint64_t rounded = ((static_cast<uint64_t>(raw) + 50) / 100) * 100;
        if (rounded > UINT32_MAX) throw Error(ERR_FAILED, "voltage response overflow");
        const auto value = static_cast<uint32_t>(rounded);
        std::memcpy(output, &value, sizeof(value));
    });
}
}
