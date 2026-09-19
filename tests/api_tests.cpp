#include "mongoose/j2534.h"
#include "replay.hpp"
#include <array>
#include <iostream>
#include <cstring>
namespace mongoose {
// Link-time injection in this test executable only; production resolves a real backend.
// Only device acquisition is stubbed -- parse_selector below is the production parser,
// so the invalid-name case still exercises real code.
std::unique_ptr<Transport> open_transport(const Selector &, Trace) {
    std::deque<Exchange> exchanges;
    uint16_t seq = 0;
    for (uint16_t opcode : std::array<uint16_t, 6>{0x103, 3, 0x109, 0xc, 0xc, 5}) {
        ++seq; Bytes body(opcode == 0xc ? 28 : 20, 0); put16(body, 2, 1); put16(body, 4, opcode | 0x8000); put16(body, 6, seq);
        Bytes payload;
        if (opcode == 0xc) {
            const uint8_t selector = seq == 4 ? 3 : 0x2b;
            payload = {selector, 0, 0, 0}; body[20] = selector;
            if (selector == 0x2b) { body[25] = 0x10; body[26] = 1; body[27] = 1; }
        }
        exchanges.push_back({encode(request(opcode, seq, payload)), {encode(body)}});
    }
    return std::make_unique<Replay>(std::move(exchanges));
}
}
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
int main() {
    try {
        CHECK(PassThruOpen(nullptr, nullptr) == ERR_NULL_PARAMETER);
        CHECK(PassThruClose(0) == ERR_INVALID_DEVICE_ID);
        char message[80]; CHECK(PassThruGetLastError(message) == 0); CHECK(std::strstr(message, "invalid device"));
        uint32_t device = 99;
        char invalid[] = "not-a-selector";
        CHECK(PassThruOpen(invalid, &device) == ERR_FAILED && device == 0);
        uint32_t previous = 0;
        for (unsigned i = 0; i < 100; ++i) {
            CHECK(PassThruOpen(nullptr, &device) == 0 && device > previous); previous = device;
            CHECK(PassThruGetLastError(message) == 0); CHECK(message[0] == '\0'); // cleared on success
            uint32_t channel = 99;
            CHECK(PassThruConnect(device, ISO14230, 0, 500000, &channel) == ERR_NOT_SUPPORTED && channel == 0);
            CHECK(PassThruConnect(device, 1234, 0, 500000, &channel) == ERR_INVALID_PROTOCOL_ID);
            CHECK(PassThruSetProgrammingVoltage(device, 13, 18000) == ERR_NOT_SUPPORTED);
            uint32_t voltage = 99;
            CHECK(PassThruIoctl(device, READ_VBATT, nullptr, &voltage) == 0 && voltage == 0);
            char fw[80] = "invalid", dll[80] = "invalid", api[80] = "invalid";
            CHECK(PassThruReadVersion(device, fw, dll, api) == 0);
            CHECK(std::strcmp(fw, "1.1.16.0") == 0 && std::strcmp(api, "04.04") == 0);
            CHECK(PassThruClose(device) == 0);
            CHECK(PassThruClose(device) == ERR_INVALID_DEVICE_ID);
        }
        // J2534-1 04.04 argument and handle rules that need no adapter: every NULL pointer the spec names, an
        // invalid handle for each function, and the error codes for identifiers the driver does not know.
        CHECK(PassThruGetLastError(nullptr) == ERR_NULL_PARAMETER);
        CHECK(PassThruReadVersion(1, nullptr, message, message) == ERR_NULL_PARAMETER);
        CHECK(PassThruReadVersion(1, message, nullptr, message) == ERR_NULL_PARAMETER);
        CHECK(PassThruReadVersion(1, message, message, nullptr) == ERR_NULL_PARAMETER);
        CHECK(PassThruReadVersion(12345, message, message, message) == ERR_INVALID_DEVICE_ID);
        CHECK(PassThruConnect(12345, CAN, 0, 500000, &device) == ERR_INVALID_DEVICE_ID);
        CHECK(PassThruConnect(1, CAN, 0, 500000, nullptr) == ERR_NULL_PARAMETER);
        CHECK(PassThruSetProgrammingVoltage(12345, 13, 18000) == ERR_INVALID_DEVICE_ID);
        {
            uint32_t opened = 0; CHECK(PassThruOpen(nullptr, &opened) == 0);
            uint32_t channel = 99;
            CHECK(PassThruConnect(opened, 0, 0, 500000, &channel) == ERR_INVALID_PROTOCOL_ID && channel == 0);   // protocol 0 is not one
            CHECK(PassThruConnect(opened, ISO15765 + 1, 0, 500000, &channel) == ERR_INVALID_PROTOCOL_ID);
            CHECK(PassThruConnect(opened, CAN, 0, 0, &channel) == ERR_INVALID_BAUDRATE && channel == 0);
            CHECK(PassThruConnect(opened, J1850VPW, 0, 10400, &channel) == ERR_NOT_SUPPORTED);
            CHECK(PassThruConnect(opened, ISO9141, 0, 10400, &channel) == ERR_NOT_SUPPORTED);
            // Ioctl: an unknown ID, or one that does not apply to CAN, is ERR_INVALID_IOCTL_ID before the handle is
            // looked at, as the vendor does; a known one with a bad handle names the handle.
            for (uint32_t ioctl : {0u, 6u, 11u, 12u, 13u, 15u, 0x8000u, 0x10102u, 0xffffffffu, static_cast<uint32_t>(FIVE_BAUD_INIT), static_cast<uint32_t>(FAST_INIT)}) {
                CHECK(PassThruIoctl(opened, ioctl, nullptr, nullptr) == ERR_INVALID_IOCTL_ID);
                CHECK(PassThruIoctl(12345, ioctl, nullptr, nullptr) == ERR_INVALID_IOCTL_ID);
            }
            CHECK(PassThruIoctl(12345, READ_VBATT, nullptr, &device) == ERR_INVALID_DEVICE_ID);
            CHECK(PassThruIoctl(opened, READ_VBATT, nullptr, nullptr) == ERR_NULL_PARAMETER);
            for (uint32_t ioctl : {static_cast<uint32_t>(GET_CONFIG), static_cast<uint32_t>(SET_CONFIG), static_cast<uint32_t>(CLEAR_TX_BUFFER),
                                   static_cast<uint32_t>(CLEAR_RX_BUFFER), static_cast<uint32_t>(CLEAR_PERIODIC_MSGS), static_cast<uint32_t>(CLEAR_MSG_FILTERS)})
                CHECK(PassThruIoctl(opened, ioctl, nullptr, nullptr) == ERR_INVALID_CHANNEL_ID);   // a device is not a channel
            // Error text: 80 bytes at most, always terminated, and cleared by a call that succeeds.
            CHECK(PassThruConnect(opened, 0, 0, 500000, &channel) != 0);
            char text[80]; std::memset(text, 'x', sizeof(text));
            CHECK(PassThruGetLastError(text) == 0 && std::memchr(text, '\0', sizeof(text)) && std::strlen(text) > 0);
            // The error text belongs to the last failing call; the next call replaces it. (ReadVersion runs
            // first only because the scripted adapter expects its two reads before the close.)
            uint32_t battery = 0;
            CHECK(PassThruIoctl(opened, READ_VBATT, nullptr, &battery) == 0);   // the scripted order: voltage, then version
            char firmware[80], driver[80], api[80];
            CHECK(PassThruReadVersion(opened, firmware, driver, api) == 0);
            CHECK(std::strlen(driver) > 0 && std::strlen(driver) < 80 && std::strlen(api) < 80);
            CHECK(PassThruClose(opened) == 0);
            CHECK(PassThruGetLastError(text) == 0 && text[0] == '\0');
        }
        PASSTHRU_MSG message_buffer{}; uint32_t count = 2, id = 5;
        CHECK(PassThruReadMsgs(1, &message_buffer, &count, 0) == ERR_INVALID_CHANNEL_ID && count == 0);
        count = 2;
        CHECK(PassThruWriteMsgs(1, nullptr, &count, 0) == ERR_NULL_PARAMETER && count == 0);
        CHECK(PassThruStartPeriodicMsg(1, &message_buffer, &id, 10) == ERR_INVALID_CHANNEL_ID && id == 0);
        id = 5;
        CHECK(PassThruStartMsgFilter(1, PASS_FILTER, &message_buffer, &message_buffer, nullptr, &id) == ERR_INVALID_CHANNEL_ID && id == 0);
        CHECK(PassThruDisconnect(1) == ERR_INVALID_CHANNEL_ID);
        CHECK(PassThruStopPeriodicMsg(1, 1) == ERR_INVALID_CHANNEL_ID);
        CHECK(PassThruStopMsgFilter(1, 1) == ERR_INVALID_CHANNEL_ID);
        std::cout << "100 synthetic lifecycle cycles and API errors passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
