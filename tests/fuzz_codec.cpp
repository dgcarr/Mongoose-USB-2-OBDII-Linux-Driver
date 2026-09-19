#include "codec.hpp"
#include "can.hpp"
#include "config.hpp"
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
            const auto payload = mongoose::can_transmit(outbound, data[2]);
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
    // The encoders added since take the same untrusted message fields: periodic (interval and size),
    // block and pass filters, and the ISO15765 transmit, which now accepts up to 4095 data bytes.
    if (size >= 8) {
        PASSTHRU_MSG probe{};
        probe.ProtocolID = data[0] & 1 ? static_cast<uint32_t>(CAN) : static_cast<uint32_t>(ISO15765);
        probe.TxFlags = data[1] & 3 ? static_cast<uint32_t>(CAN_29BIT_ID) : 0;
        probe.DataSize = static_cast<uint32_t>(data[2]) | (static_cast<uint32_t>(data[3] & 0x1f) << 8);
        const uint32_t interval = static_cast<uint32_t>(data[4]) | static_cast<uint32_t>(data[5]) << 8 | static_cast<uint32_t>(data[6] & 1) << 16;
        std::copy_n(data, std::min<size_t>(size, 64), probe.Data);
        try {
            const auto periodic = mongoose::can_periodic(probe, interval);
            if (periodic.size() != size_t{13} + probe.DataSize || periodic[0] != mongoose::table_periodic) std::abort();
        } catch (const mongoose::Error &) {}
        try {
            const auto transmit = mongoose::isotp_transmit(probe, interval);
            if (transmit.size() != size_t{12} + probe.DataSize) std::abort();
        } catch (const mongoose::Error &) {}
        PASSTHRU_MSG pattern = probe;
        try { (void)mongoose::can_block_filter(probe, pattern, probe.TxFlags); } catch (const mongoose::Error &) {}
        try { (void)mongoose::can_pass_filter(probe, pattern, probe.TxFlags); } catch (const mongoose::Error &) {}
        // The configuration table and validator take a protocol, an ID and a value from the caller.
        const uint32_t parameter = static_cast<uint32_t>(data[4]) | static_cast<uint32_t>(data[5]) << 8 |
                                   static_cast<uint32_t>(data[6]) << 16 | static_cast<uint32_t>(data[7] & 0x81) << 24;
        const uint32_t value = static_cast<uint32_t>(data[1]) | static_cast<uint32_t>(data[2]) << 8 | static_cast<uint32_t>(data[3]) << 16;
        for (const uint16_t protocol : {static_cast<uint16_t>(CAN), static_cast<uint16_t>(ISO15765), static_cast<uint16_t>(data[0])}) {
            try { (void)mongoose::config_route(protocol, parameter); } catch (const mongoose::Error &) {}
            try { mongoose::config_validate_set(protocol, parameter, value); } catch (const mongoose::Error &) {}
        }
    }
    // Sequence-paired transmit confirmations and loopback, on an ISO15765 receiver and a CAN one, driven by
    // the same frames: recorded requests, indications with arbitrary sequences, and a flag that flips.
    if (size >= 6) {
        for (const uint16_t protocol : {static_cast<uint16_t>(ISO15765), static_cast<uint16_t>(CAN)}) {
            mongoose::CanReceiver paired(protocol);
            paired.set_loopback(data[0] & 1);
            const auto call = std::make_shared<mongoose::CanReceiver::TransmitCount>();
            for (size_t i = 1; i + 2 < size; i += 3) {
                PASSTHRU_MSG sent{};
                sent.ProtocolID = protocol; sent.TxFlags = data[i] & 1 ? static_cast<uint32_t>(CAN_29BIT_ID) : 0;
                sent.DataSize = 4 + (data[i + 1] & 0x0f);
                std::copy_n(data, std::min<size_t>(size, 16), sent.Data);
                paired.note_transmit(sent, data[i + 2], call);
                mongoose::Bytes body(20, 0);
                mongoose::put16(body, 2, mongoose::channel_node(protocol)); mongoose::put16(body, 4, 10);
                mongoose::put16(body, 6, data[(i + 5) % size]); mongoose::put16(body, 12, 0x106);
                mongoose::put16(body, 16, static_cast<uint16_t>(i));
                paired.receive(body);
                if (data[i] & 8) paired.forget_transmit();
                if (data[i] & 16) paired.set_loopback(!paired.loopback());
                if (data[i] & 32) paired.forget_transmits();
                if (data[i] & 64) paired.flush_receive();
            }
            // A call's own count can never exceed what the channel confirmed in total.
            if (paired.confirmed(call) > paired.transmitted()) std::abort();
            PASSTHRU_MSG drained[8]{}; uint32_t got = 0;
            try { paired.read(drained, 8, got, 0); } catch (const mongoose::Error &) {}
            for (uint32_t i = 0; i < got; ++i)
                if (drained[i].DataSize > 4 + 4095 || drained[i].DataSize < 4) std::abort();
        }
    }
    PASSTHRU_MSG message{};
    uint32_t count = 0;
    try { receiver.read(&message, 1, count, 0); } catch (const mongoose::Error &) {}
    if (count && (message.DataSize < 4 || message.DataSize > 12 || message.ExtraDataIndex != message.DataSize)) std::abort();
    return 0;
}
