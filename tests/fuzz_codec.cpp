#include "codec.hpp"
#include "can.hpp"
#include "isotp.hpp"
#include <algorithm>
#include <cstdlib>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mongoose::Decoder decoder;
    mongoose::CanReceiver receiver;
    for (size_t offset = 0; offset < size;) {
        const size_t length = std::min(size-offset, static_cast<size_t>(1+data[offset]));
        auto frames = decoder.feed(std::span(data+offset, length));
        if (decoder.buffered() > mongoose::max_body+3) std::abort();
        for (const auto &body : frames) {
            receiver.receive(body);
            mongoose::Decoder check;
            auto roundtrip = check.feed(mongoose::encode(body));
            if (roundtrip.size() != 1 || roundtrip[0] != body) std::abort();
        }
        offset += length;
    }
    // The transmit encoder copies DataSize bytes out of a caller-supplied message, so
    // drive its size and flag fields from the same untrusted input.
    if (size >= 3) {
        PASSTHRU_MSG outbound{};
        outbound.ProtocolID = CAN;
        outbound.TxFlags = data[0] & 1 ? static_cast<uint32_t>(CAN_29BIT_ID) : 0;
        outbound.DataSize = data[1];
        outbound.ExtraDataIndex = data[2];
        try {
            const auto payload = mongoose::can_transmit(outbound, outbound.TxFlags, data[2]);
            if (payload.size() != size_t{12} + outbound.DataSize) std::abort();
        } catch (const mongoose::Error &) {}
    }
    // Reassembly consumes attacker-shaped frame sequences: lengths, sequence numbers and
    // truncation all come from the wire, and a partial message persists across frames.
    mongoose::IsoTpReassembler reassembler;
    for (size_t offset = 0; offset + 1 < size;) {
        const size_t length = std::min(size-offset-1, static_cast<size_t>(1+(data[offset] & 0x1f)));
        for (const auto &output : reassembler.feed(std::span(data+offset+1, length)))
            if (output.data.size() > 4 + mongoose::IsoTpReassembler::max_message) std::abort();
        offset += 1 + length;
    }
    PASSTHRU_MSG message{};
    uint32_t count = 0;
    try { receiver.read(&message, 1, count, 0); } catch (const mongoose::Error &) {}
    if (count && (message.DataSize < 4 || message.DataSize > 12 || message.ExtraDataIndex != message.DataSize)) std::abort();
    return 0;
}
