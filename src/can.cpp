#include "can.hpp"
#include <algorithm>
#include <utility>
namespace mongoose {
namespace {
Bytes can_table_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern, uint32_t channel_flags,
                       uint8_t table, uint8_t type) {
    if (mask.ProtocolID != CAN || pattern.ProtocolID != CAN)
        throw Error(ERR_MSG_PROTOCOL_ID, "filter protocol must match CAN channel");
    if ((mask.TxFlags | pattern.TxFlags) & ~static_cast<uint32_t>(CAN_29BIT_ID))
        throw Error(ERR_INVALID_FLAGS, "unsupported CAN filter flags");
    if (mask.TxFlags != channel_flags || pattern.TxFlags != channel_flags)
        throw Error(ERR_INVALID_MSG, "filter identifier width must match channel");
    if (mask.DataSize < 4 || mask.DataSize > 12 || pattern.DataSize != mask.DataSize)
        throw Error(ERR_INVALID_MSG, "CAN mask and pattern must have equal sizes in 4..12");
    // 1000e600: u32 table selector, u16 flags, u8 type, u8 pattern size, followed by mask and
    // pattern verbatim (including big-endian CAN IDs). PASS is selector 0 / type 1 and BLOCK is
    // selector 1 / type 2.
    Bytes payload{table,0,0,0,0,0,type,static_cast<uint8_t>(pattern.DataSize)};
    put16(payload, 4, static_cast<uint16_t>(mask.TxFlags));
    payload.insert(payload.end(), mask.Data, mask.Data + mask.DataSize);
    payload.insert(payload.end(), pattern.Data, pattern.Data + pattern.DataSize);
    return payload;
}
}
Bytes can_pass_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern, uint32_t channel_flags) {
    return can_table_filter(mask, pattern, channel_flags, table_pass, 1);
}
Bytes can_block_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern, uint32_t channel_flags) {
    return can_table_filter(mask, pattern, channel_flags, table_block, 2);
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
Bytes can_periodic(const PASSTHRU_MSG &message, uint32_t channel_flags, uint32_t interval_ms) {
    if (interval_ms < 5 || interval_ms > 65535)
        throw Error(ERR_INVALID_TIME_INTERVAL, "periodic interval must be 5..65535 ms");
    if (message.ProtocolID != CAN)
        throw Error(ERR_MSG_PROTOCOL_ID, "message protocol must match CAN channel");
    const uint32_t flags = message.TxFlags ? message.TxFlags : channel_flags;
    if (flags & ~static_cast<uint32_t>(CAN_29BIT_ID))
        throw Error(ERR_INVALID_FLAGS, "unsupported CAN transmit flags");
    if (message.DataSize < 4 || message.DataSize > 12)
        throw Error(ERR_INVALID_MSG, "CAN message size must be 4..12");
    Bytes payload(13, 0);
    payload[0] = table_periodic;
    put16(payload, 4, static_cast<uint16_t>(interval_ms)); put16(payload, 6, static_cast<uint16_t>(interval_ms >> 16));
    put16(payload, 8, static_cast<uint16_t>(flags)); put16(payload, 10, static_cast<uint16_t>(flags >> 16));
    payload[12] = static_cast<uint8_t>(message.DataSize);
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
    // The whole message goes to the adapter in one cOutboundData, ID first. The vendor never splits
    // it (its write path, 1000c270 and the frame builder 1006b090, copies DataSize bytes as given), so
    // segmentation is the adapter's: its firmware carries the ISO-TP timing parameters, N_As/N_Bs/N_Cs
    // and so on. ISO 15765-2 allows up to 4095 payload bytes.
    if (message.DataSize < 5 || message.DataSize > 4 + 4095)
        throw Error(ERR_INVALID_MSG, "ISO15765 message must be an ID plus 1..4095 data bytes");
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
            // Only a confirmation that pairs with a write recorded here counts toward WriteMsgs. A running
            // periodic message is confirmed too, and must not be mistaken for a timed write finishing.
            if (deliver_confirmed(le16(body, 6), le32(body, 16))) ++transmitted_;
            ready_.notify_all();
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
void CanReceiver::note_transmit(const PASSTHRU_MSG &message, uint16_t sequence) {
    std::lock_guard lock(mutex_);
    // A bus that never confirms (nothing attached) leaves every record unpaired; the oldest is the
    // one least likely to be confirmed, so it goes rather than failing the write.
    if (pending_transmits_.size() == message_capacity) pending_transmits_.pop_front();
    const auto size = std::min<size_t>(message.DataSize, 4 + 8 + 4096);
    pending_transmits_.push_back({sequence, message.TxFlags, Bytes(message.Data, message.Data + size)});
}
void CanReceiver::set_loopback(bool enabled) {
    std::lock_guard lock(mutex_);
    loopback_ = enabled;
}
bool CanReceiver::loopback() {
    std::lock_guard lock(mutex_);
    return loopback_;
}
// Called with mutex_ held when iMsgTxDone arrives. Mirrors vendor 1000bcb0 case 0x106: find the record
// for the echoed sequence, discarding older ones that were never confirmed; an unknown sequence
// leaves the records alone. ISO15765 always gets the TX_MSG_TYPE|TX_DONE message with the ID alone
// (Windows captures E1, E2). With LOOPBACK set, either protocol also gets the frame back as a received
// message, RxStatus TX_MSG_TYPE, after it.
bool CanReceiver::deliver_confirmed(uint16_t sequence, uint32_t timestamp) {
    const auto found = std::find_if(pending_transmits_.begin(), pending_transmits_.end(),
                                    [sequence](const PendingTransmit &p) { return p.sequence == sequence; });
    if (found == pending_transmits_.end()) return false;
    const PendingTransmit sent = std::move(*found);
    pending_transmits_.erase(pending_transmits_.begin(), found + 1);
    if (protocol_ == ISO15765) {
        if (sent.message.size() >= 4) {
            if (messages_.size() == message_capacity) { overflow_ = true; return true; }
            messages_.push_back({static_cast<uint32_t>(TX_MSG_TYPE | TX_DONE), timestamp,
                                 Bytes(sent.message.begin(), sent.message.begin() + 4), sent.flags});
        }
        if (loopback_) {
            if (messages_.size() == message_capacity) { overflow_ = true; return true; }
            messages_.push_back({static_cast<uint32_t>(TX_MSG_TYPE), timestamp, sent.message, sent.flags});
        }
    } else if (loopback_ && sent.message.size() >= 4 && sent.message.size() <= 12) {
        if (frames_.size() == capacity) { overflow_ = true; return true; }
        Frame frame{static_cast<uint32_t>(TX_MSG_TYPE) | (sent.flags & CAN_29BIT_ID ? 0x100U : 0U), timestamp,
                    static_cast<uint16_t>(sent.message.size()), {}, sent.flags};
        std::copy(sent.message.begin(), sent.message.end(), frame.data.begin());
        frames_.push_back(frame);
    }
    return true;
}
void CanReceiver::forget_transmit() {
    std::lock_guard lock(mutex_);
    if (!pending_transmits_.empty()) pending_transmits_.pop_back();
}
void CanReceiver::flush_receive() {
    std::lock_guard lock(mutex_);
    frames_.clear(); messages_.clear(); reassembler_.reset();
    overflow_ = malformed_ = false;
}
void CanReceiver::forget_transmits() {
    std::lock_guard lock(mutex_);
    pending_transmits_.clear();
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
    frames_.clear(); messages_.clear(); pending_transmits_.clear(); ready_.notify_all();
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
            out.TxFlags = message.tx_flags;
            out.DataSize = static_cast<uint32_t>(message.data.size());
            // An indication (start of message, transmit done) carries the ID alone and reports
            // no extra data, as in the vendor logs; a real message reports its own length.
            out.ExtraDataIndex = (message.status & (START_OF_MESSAGE | TX_DONE)) ? 0 : out.DataSize;
            std::copy(message.data.begin(), message.data.end(), out.Data);
        }
        while (!frames_.empty() && count < requested) {
            const auto frame = frames_.front(); frames_.pop_front();
            auto &message = messages[count++];
            message = {};
            message.ProtocolID = CAN; message.RxStatus = frame.status; message.Timestamp = frame.timestamp;
            message.TxFlags = frame.tx_flags;
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
