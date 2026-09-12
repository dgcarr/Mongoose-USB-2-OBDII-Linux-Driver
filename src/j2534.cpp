#include "mongoose/j2534.h"
#include "session.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
namespace {
using namespace mongoose;
thread_local std::array<char, 80> last_error{};
std::mutex devices_mutex;
std::map<uint32_t, std::shared_ptr<Session>> devices;
uint64_t next_device = 1;
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
std::shared_ptr<Session> device(uint32_t id) {
    std::lock_guard lock(devices_mutex);
    auto found = devices.find(id);
    if (found == devices.end()) throw Error(ERR_INVALID_DEVICE_ID, "invalid device ID");
    return found->second;
}
[[noreturn]] void unsupported(const char *operation) {
    throw Error(ERR_NOT_SUPPORTED, std::string(operation) + ": awaiting protocol/hardware validation");
}
[[noreturn]] void no_channel() {
    throw Error(ERR_INVALID_CHANNEL_ID, "no channel allocated; Connect is not yet supported");
}
void accepted(std::span<const uint8_t> response, bool start = false) {
    const uint32_t status = Session::status(response);
    if (status == 0 || (start && status == 7)) return;
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
        std::string serial;
        if (name) {
            const char *text = static_cast<const char *>(name);
            const size_t size = strnlen(text, 256);
            if (size == 256 || size <= 7 || std::strncmp(text, "serial:", 7) != 0)
                throw Error(ERR_FAILED, "Open name must be NULL or serial:<USB serial>");
            serial.assign(text+7, size-7);
        }
        auto session = std::make_shared<Session>(usb_transport(serial));
        accepted(session->command(0x103), true);
        accepted(session->command(3));
        try {
            accepted(session->command(0x109));
            std::lock_guard lock(devices_mutex);
            if (next_device > std::numeric_limits<uint32_t>::max()) throw Error(ERR_EXCEEDED_LIMIT, "device IDs exhausted");
            const auto allocated = static_cast<uint32_t>(next_device++);
            devices.emplace(allocated, session); *id = allocated;
        } catch (...) {
            try { session->command(5); } catch (...) {}
            throw;
        }
    });
}
int32_t J2534_CALL PassThruClose(uint32_t id) {
    return guarded([&] {
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(devices_mutex);
            auto found = devices.find(id);
            if (found == devices.end()) throw Error(ERR_INVALID_DEVICE_ID, "invalid device ID");
            session = std::move(found->second); devices.erase(found);
        }
        std::exception_ptr error;
        try { accepted(session->command(5)); } catch (...) { error = std::current_exception(); }
        try { session->close(); } catch (...) { if (!error) error = std::current_exception(); }
        if (error) std::rethrow_exception(error);
    });
}
int32_t J2534_CALL PassThruConnect(uint32_t id, uint32_t protocol, uint32_t, uint32_t baud, uint32_t *channel) {
    return guarded([&] {
        required(channel); *channel = 0; device(id);
        if (protocol < J1850VPW || protocol > ISO15765) throw Error(ERR_INVALID_PROTOCOL_ID, "unknown protocol ID");
        if (!baud) throw Error(ERR_INVALID_BAUDRATE, "baud rate is zero");
        unsupported("channel setup");
    });
}
int32_t J2534_CALL PassThruDisconnect(uint32_t) { return guarded([] { no_channel(); }); }
int32_t J2534_CALL PassThruReadMsgs(uint32_t, PASSTHRU_MSG *messages, uint32_t *count, uint32_t) {
    return guarded([&] { required(count); *count = 0; required(messages); no_channel(); });
}
int32_t J2534_CALL PassThruWriteMsgs(uint32_t, PASSTHRU_MSG *messages, uint32_t *count, uint32_t) {
    return guarded([&] { required(count); *count = 0; required(messages); no_channel(); });
}
int32_t J2534_CALL PassThruStartPeriodicMsg(uint32_t, PASSTHRU_MSG *message, uint32_t *id, uint32_t) {
    return guarded([&] { required(id); *id = 0; required(message); no_channel(); });
}
int32_t J2534_CALL PassThruStopPeriodicMsg(uint32_t, uint32_t) { return guarded([] { no_channel(); }); }
int32_t J2534_CALL PassThruStartMsgFilter(uint32_t, uint32_t, PASSTHRU_MSG *mask, PASSTHRU_MSG *pattern, PASSTHRU_MSG *, uint32_t *id) {
    return guarded([&] { required(id); *id = 0; required(mask); required(pattern); no_channel(); });
}
int32_t J2534_CALL PassThruStopMsgFilter(uint32_t, uint32_t) { return guarded([] { no_channel(); }); }
int32_t J2534_CALL PassThruSetProgrammingVoltage(uint32_t id, uint32_t, uint32_t) {
    return guarded([&] { device(id); unsupported("programming voltage"); });
}
int32_t J2534_CALL PassThruReadVersion(uint32_t id, char *firmware, char *dll, char *api) {
    return guarded([&] {
        required(firmware); required(dll); required(api);
        firmware[0] = dll[0] = api[0] = '\0';
        auto session = device(id);
        const uint32_t version = get_value(*session, 0x2b);
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
        const uint32_t raw = get_value(*session, ioctl_id == READ_VBATT ? 3 : 2);
        // Vendor 10059d60 rounds millivolts to the nearest 100, ties upward.
        const uint64_t rounded = ((static_cast<uint64_t>(raw) + 50) / 100) * 100;
        if (rounded > UINT32_MAX) throw Error(ERR_FAILED, "voltage response overflow");
        const auto value = static_cast<uint32_t>(rounded);
        std::memcpy(output, &value, sizeof(value));
    });
}
}
