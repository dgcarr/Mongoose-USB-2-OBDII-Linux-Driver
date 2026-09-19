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
