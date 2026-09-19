#include "can.hpp"
#include <algorithm>
#include <utility>
namespace mongoose {
Bytes can_pass_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern, uint32_t channel_flags) {
    if (mask.ProtocolID != CAN || pattern.ProtocolID != CAN)
        throw Error(ERR_MSG_PROTOCOL_ID, "filter protocol must match CAN channel");
    if ((mask.TxFlags | pattern.TxFlags) & ~static_cast<uint32_t>(CAN_29BIT_ID))
        throw Error(ERR_INVALID_FLAGS, "unsupported CAN filter flags");
    if (mask.TxFlags != channel_flags || pattern.TxFlags != channel_flags)
        throw Error(ERR_INVALID_MSG, "filter identifier width must match channel");
    if (mask.DataSize < 4 || mask.DataSize > 12 || pattern.DataSize != mask.DataSize)
        throw Error(ERR_INVALID_MSG, "CAN mask and pattern must have equal sizes in 4..12");
    // 1000e600: selector 0, u16 flags, u8 PASS type, u8 pattern size,
    // followed by mask and pattern verbatim (including big-endian CAN IDs).
    Bytes payload{0,0,0,0,0,0,1,static_cast<uint8_t>(pattern.DataSize)};
    put16(payload, 4, static_cast<uint16_t>(mask.TxFlags));
    payload.insert(payload.end(), mask.Data, mask.Data + mask.DataSize);
    payload.insert(payload.end(), pattern.Data, pattern.Data + pattern.DataSize);
    return payload;
}
Bytes can_transmit(const PASSTHRU_MSG &message, uint32_t channel_flags, uint32_t timeout_ms) {
    if (message.ProtocolID != CAN)
        throw Error(ERR_MSG_PROTOCOL_ID, "message protocol must match CAN channel");
    // ConnectFlags sets default identifier width; individual messages may override CAN_29BIT_ID.
    const uint32_t flags = message.TxFlags ? message.TxFlags : channel_flags;
    if (flags & ~static_cast<uint32_t>(CAN_29BIT_ID))
        throw Error(ERR_INVALID_FLAGS, "unsupported CAN transmit flags");
    // Four ID bytes plus up to eight data bytes; the ID is big-endian, as on receive.
    if (message.DataSize < 4 || message.DataSize > 12)
        throw Error(ERR_INVALID_MSG, "CAN message size must be 4..12");
    // 1006b090: +12 u32 TxFlags, +16 u32 timeout, +20 u16 DataSize, +22 u16
    // ExtraDataIndex, +24 data. The Windows capture names +16, which the static pass
    // could only call an unresolved caller argument.
    Bytes payload(12, 0);
    put16(payload, 0, static_cast<uint16_t>(flags));
    put16(payload, 2, static_cast<uint16_t>(flags >> 16));
    put16(payload, 4, static_cast<uint16_t>(timeout_ms));
    put16(payload, 6, static_cast<uint16_t>(timeout_ms >> 16));
    put16(payload, 8, static_cast<uint16_t>(message.DataSize));
    put16(payload, 10, static_cast<uint16_t>(message.ExtraDataIndex));
    payload.insert(payload.end(), message.Data, message.Data + message.DataSize);
    return payload;
}
Bytes isotp_flow_control_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern,
                                const PASSTHRU_MSG &flow) {
    for (const auto *message : {&mask, &pattern, &flow})
        if (message->ProtocolID != ISO15765)
            throw Error(ERR_MSG_PROTOCOL_ID, "filter protocol must match ISO15765 channel");
    const uint32_t flags = pattern.TxFlags;
    if (mask.TxFlags != flags || flow.TxFlags != flags)
        throw Error(ERR_INVALID_MSG, "mask, pattern and flow-control flags must agree");
    if (flags & ~static_cast<uint32_t>(ISO15765_FRAME_PAD))
        throw Error(ERR_INVALID_FLAGS, "unsupported ISO15765 filter flags");
    for (const auto *message : {&mask, &pattern, &flow})
        if (message->DataSize != 4) throw Error(ERR_INVALID_MSG, "ISO15765 filter messages are four ID bytes");
    const auto identifier = [](const PASSTHRU_MSG &message) {
        return static_cast<uint32_t>(message.Data[0]) << 24 | static_cast<uint32_t>(message.Data[1]) << 16 |
               static_cast<uint32_t>(message.Data[2]) << 8 | message.Data[3];
    };
    if ((identifier(mask) & 0x7ff) != 0x7ff)
        throw Error(ERR_INVALID_MSG, "the adapter matches the whole 11-bit ID; mask must cover it");
    if (identifier(pattern) > 0x7ff || identifier(flow) > 0x7ff)
        throw Error(ERR_INVALID_MSG, "only 11-bit ISO15765 identifiers are supported");
    // 02000000 40000000 00 000007e8 00 000007e0 00, byte for byte as the vendor sent it.
    Bytes payload{2,0,0,0, 0,0,0,0, 0, 0,0,0,0, 0, 0,0,0,0, 0};
    put16(payload, 4, static_cast<uint16_t>(flags));
    put16(payload, 6, static_cast<uint16_t>(flags >> 16));
    std::copy_n(pattern.Data, 4, payload.begin() + 9);
    std::copy_n(flow.Data, 4, payload.begin() + 14);
    return payload;
}
Bytes isotp_transmit(const PASSTHRU_MSG &message, uint32_t timeout_ms) {
    if (message.ProtocolID != ISO15765)
        throw Error(ERR_MSG_PROTOCOL_ID, "message protocol must match ISO15765 channel");
    if (message.TxFlags & ~static_cast<uint32_t>(ISO15765_FRAME_PAD))
        throw Error(ERR_INVALID_FLAGS, "unsupported ISO15765 transmit flags");
    // ID plus up to seven service bytes fits one CAN frame; longer needs the adapter to
    // segment, which no capture shows.
    if (message.DataSize < 5 || message.DataSize > 11)
        throw Error(message.DataSize > 11 ? ERR_NOT_SUPPORTED : ERR_INVALID_MSG,
                    "ISO15765 transmit supports one single frame: ID plus 1..7 data bytes");
    Bytes payload(12, 0);
    put16(payload, 0, static_cast<uint16_t>(message.TxFlags));
    put16(payload, 2, static_cast<uint16_t>(message.TxFlags >> 16));
    put16(payload, 4, static_cast<uint16_t>(timeout_ms));
    put16(payload, 6, static_cast<uint16_t>(timeout_ms >> 16));
    put16(payload, 8, static_cast<uint16_t>(message.DataSize));
    put16(payload, 10, 0);
    payload.insert(payload.end(), message.Data, message.Data + message.DataSize);
    return payload;
}
void CanReceiver::receive(std::span<const uint8_t> body) {
    std::lock_guard lock(mutex_);
    if (stopped_ || body.size() < 12 || le16(body, 0) != 0 || le16(body, 2) != channel_node(protocol_)) return;
    const auto opcode = le16(body, 4);
    if (opcode == 10) {
        if (body.size() < 20) return;
        const auto indication = le32(body, 12);
        // 1000bcb0 reports these two firmware loss indications as buffer overflow.
        if (indication == 0x10b || indication == 0x119) {
            overflow_ = true; ready_.notify_all();
        } else if (indication == 0x106) {
            // iMsgTxDone: the adapter confirming one frame left the controller.
            ++transmitted_; ready_.notify_all();
        }
        return;
    }
    if (opcode != 9) return;
    if (body.size() < 24 || le16(body, 22) < 4 || le16(body, 22) > 12 ||
        body.size() != size_t{24} + le16(body, 22)) {
        malformed_ = true; ready_.notify_all(); return;
    }
    if (protocol_ == ISO15765) {
        const uint32_t timestamp = le32(body, 16);
        for (auto &output : reassembler_.feed(body.subspan(24, le16(body, 22)))) {
            if (messages_.size() == message_capacity) { overflow_ = true; break; }
            messages_.push_back({output.rx_status, timestamp, std::move(output.data)});
        }
        ready_.notify_all();
        return;
    }
    if (frames_.size() == capacity) {
        overflow_ = true; ready_.notify_all(); return; // preserve queued frames; drop newest
    }
    // cHSCAN vtable+0x94 -> 1001a130 -> 1001e5b0 returns 0xc0000100.
    // 1000b790 ignores body+20, uses +22 for DataSize and sets ExtraDataIndex=size.
    Frame frame{le32(body, 12) & 0xc0000100U, le32(body, 16), le16(body, 22), {}};
    std::copy_n(body.begin() + 24, frame.size, frame.data.begin());
    frames_.push_back(frame);
    ready_.notify_all();
}
size_t CanReceiver::transmitted() {
    std::lock_guard lock(mutex_);
    return transmitted_;
}
bool CanReceiver::await_transmitted(size_t target, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    ready_.wait_until(lock, deadline, [&] { return stopped_ || transmitted_ >= target; });
    if (stopped_) throw Error(stopped_, reason_);
    return transmitted_ >= target;
}
void CanReceiver::stop(int32_t code, const std::string &reason) {
    std::lock_guard lock(mutex_);
    if (!stopped_) { stopped_ = code; reason_ = reason; }
    frames_.clear(); messages_.clear(); ready_.notify_all();
}
void CanReceiver::read(PASSTHRU_MSG *messages, uint32_t requested, uint32_t &count, uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock lock(mutex_);
    count = 0;
    const auto pending = [&] { return protocol_ == ISO15765 ? !messages_.empty() : !frames_.empty(); };
    while (count < requested) {
        if (stopped_) throw Error(stopped_, reason_);
        while (protocol_ == ISO15765 && !messages_.empty() && count < requested) {
            auto message = std::move(messages_.front()); messages_.pop_front();
            auto &out = messages[count++];
            out = {};
            out.ProtocolID = ISO15765; out.RxStatus = message.status; out.Timestamp = message.timestamp;
            out.DataSize = static_cast<uint32_t>(message.data.size());
            out.ExtraDataIndex = out.DataSize;
            std::copy(message.data.begin(), message.data.end(), out.Data);
        }
        while (!frames_.empty() && count < requested) {
            const auto frame = frames_.front(); frames_.pop_front();
            auto &message = messages[count++];
            message = {};
            message.ProtocolID = CAN; message.RxStatus = frame.status; message.Timestamp = frame.timestamp;
            message.DataSize = frame.size; message.ExtraDataIndex = frame.size;
            std::copy_n(frame.data.begin(), frame.size, message.Data);
        }
        if (count == requested || !timeout_ms || overflow_ || malformed_) break;
        if (!ready_.wait_until(lock, deadline, [&] { return stopped_ || pending() || overflow_ || malformed_; })) break;
    }
    if (std::exchange(overflow_, false)) throw Error(ERR_BUFFER_OVERFLOW, "CAN receive buffer overflow; messages lost");
    if (std::exchange(malformed_, false)) throw Error(ERR_INVALID_MSG, "malformed CAN receive frame discarded");
    // Matches the vendor ReadMsgs wrapper: empty even after a timed wait is
    // BUFFER_EMPTY; a nonempty partial timed read is TIMEOUT, retaining count.
    if (!count) throw Error(ERR_BUFFER_EMPTY, "zero CAN messages received");
    if (timeout_ms && count < requested) throw Error(ERR_TIMEOUT, "CAN read returned fewer messages than requested");
}
}
