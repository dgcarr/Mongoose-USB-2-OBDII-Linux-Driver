#include "mongoose/j2534.h"
#include "session.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
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
    uint32_t channel = 0;
    uint32_t channel_flags = 0;
    std::shared_ptr<CanReceiver> receiver;
    std::map<uint32_t, uint32_t> filters; // public ID -> opaque firmware handle (table 0)
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
[[noreturn]] void unsupported_channel(uint32_t id) {
    auto owner = lookup(id, true);
    unsupported("channel operation");
}
void retire_channel(Device &owner) {
    owner.session->set_can_receiver({});
    owner.receiver.reset();
    owner.filters.clear();
    std::lock_guard lock(devices_mutex);
    channels.erase(owner.channel);
    owner.channel = 0;
}
void uncertain_channel(Device &owner) {
    owner.reopen_required = true;
    if (owner.receiver) owner.receiver->stop(ERR_DEVICE_NOT_CONNECTED, "channel state uncertain; reopen device");
}
// Preserve Session's distinction between failures before a write and ambiguous
// wire outcomes. Definite firmware rejection is handled separately by accepted().
Bytes channel_command(Device &owner, uint16_t opcode, std::span<const uint8_t> payload = {},
                      std::chrono::milliseconds timeout = std::chrono::seconds(10), uint16_t chan = 0) {
    try {
        return owner.session->command(opcode, payload, timeout, channel_node(CAN), chan);
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
        retire_channel(state);
        std::exception_ptr error;
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
        if (protocol != CAN) unsupported("channel protocol");
        if (flags & ~static_cast<uint32_t>(CAN_29BIT_ID)) throw Error(ERR_INVALID_FLAGS, "unsupported CAN flags");
        uint32_t allocated;
        {
            std::lock_guard lock(devices_mutex);
            allocated = allocate_handle();
        }
        Bytes payload;
        for (uint32_t word : {flags, baud})
            for (unsigned shift = 0; shift < 32; shift += 8)
                payload.push_back(static_cast<uint8_t>(word >> shift));
        auto receiver = std::make_shared<CanReceiver>();
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
        retire_channel(state);
        try { accepted(channel_command(state, 7)); state.wire_channel_open = false; }
        catch (...) { state.reopen_required = true; throw; }
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
        // The adapter is told the caller's timeout; the host still has to wait for the
        // acknowledgement, so a zero (queue-and-return) timeout keeps a response budget.
        const auto budget = std::chrono::milliseconds(timeout ? std::min(timeout, 60000u) : 1000u);
        const auto deadline = std::chrono::steady_clock::now() + budget;
        auto receiver = state.receiver;
        const auto already = receiver ? receiver->transmitted() : 0;
        for (uint32_t index = 0; index < requested; ++index) {
            const auto payload = can_transmit(messages[index], state.channel_flags, timeout);
            accepted(channel_command(state, 8, payload, budget, data_chan), Allow::Queued);
            if (!timeout) *count = index + 1;  // queue-and-return: accepted is all we claim
        }
        if (!timeout) return;
        // J2534 blocks a timed write until the messages are sent, and the adapter says
        // when that happened: one iMsgTxDone per transmitted frame. Reporting the queued
        // count here instead would claim delivery for frames that never left the
        // controller -- which is exactly what happens with no bus attached.
        if (!receiver) throw Error(ERR_DEVICE_NOT_CONNECTED, "channel has no receiver");
        const bool confirmed = receiver->await_transmitted(already + requested, deadline);
        const auto sent = receiver->transmitted() - already;
        *count = static_cast<uint32_t>(std::min<size_t>(sent, requested));
        if (!confirmed)
            throw Error(ERR_TIMEOUT, "adapter did not confirm transmission of every message");
    });
}
int32_t J2534_CALL PassThruStartPeriodicMsg(uint32_t channel, PASSTHRU_MSG *message, uint32_t *id, uint32_t) {
    return guarded([&] { required(id); *id = 0; required(message); unsupported_channel(channel); });
}
int32_t J2534_CALL PassThruStopPeriodicMsg(uint32_t channel, uint32_t) { return guarded([&] { unsupported_channel(channel); }); }
int32_t J2534_CALL PassThruStartMsgFilter(uint32_t channel, uint32_t type, PASSTHRU_MSG *mask, PASSTHRU_MSG *pattern, PASSTHRU_MSG *flow, uint32_t *id) {
    return guarded([&] {
        required(id); *id = 0; required(mask); required(pattern);
        auto owner = lookup(channel, true);
        owner.session();
        auto &state = *owner.state;
        if (type != PASS_FILTER) unsupported("only CAN PASS_FILTER is implemented");
        if (flow) throw Error(ERR_INVALID_MSG, "PASS_FILTER requires no flow-control message");
        const auto payload = can_pass_filter(*mask, *pattern, state.channel_flags);
        uint32_t allocated;
        { std::lock_guard lock(devices_mutex); allocated = allocate_handle(); }
        auto response = channel_command(state, 0x0d, payload);
        accepted(response);
        if (response.size() < 24) {
            uncertain_channel(state);
            throw Error(ERR_FAILED, "filter response missing firmware handle; reopen device");
        }
        const auto handle = le32(response, 20);
        try { state.filters.emplace(allocated, handle); }
        catch (...) {
            const auto original = std::current_exception();
            Bytes remove(8, 0); put16(remove, 4, static_cast<uint16_t>(handle)); put16(remove, 6, static_cast<uint16_t>(handle >> 16));
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
        put16(payload, 4, static_cast<uint16_t>(found->second));
        put16(payload, 6, static_cast<uint16_t>(found->second >> 16));
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
    std::memcpy(description, last_error.data(), last_error.size()); return STATUS_NOERROR;
}
int32_t J2534_CALL PassThruIoctl(uint32_t id, uint32_t ioctl_id, void *, void *output) {
    return guarded([&] {
        auto session = device(id);
        if (ioctl_id != READ_VBATT && ioctl_id != READ_PROG_VOLTAGE) unsupported("IOCTL");
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
