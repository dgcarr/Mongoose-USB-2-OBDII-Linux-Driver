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
void CanReceiver::receive(std::span<const uint8_t> body) {
    std::lock_guard lock(mutex_);
    if (stopped_ || body.size() < 12 || le16(body, 0) != 0 || le16(body, 2) != channel_node(CAN)) return;
    const auto opcode = le16(body, 4);
    if (opcode == 10) {
        // 1000bcb0 reports these two firmware loss indications as buffer overflow.
        if (body.size() >= 20 && (le32(body, 12) == 0x10b || le32(body, 12) == 0x119)) {
            overflow_ = true; ready_.notify_all();
        }
        return;
    }
    if (opcode != 9) return;
    if (body.size() < 24 || le16(body, 22) < 4 || le16(body, 22) > 12 ||
        body.size() != size_t{24} + le16(body, 22)) {
        malformed_ = true; ready_.notify_all(); return;
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
void CanReceiver::stop(int32_t code, const std::string &reason) {
    std::lock_guard lock(mutex_);
    if (!stopped_) { stopped_ = code; reason_ = reason; }
    frames_.clear(); ready_.notify_all();
}
void CanReceiver::read(PASSTHRU_MSG *messages, uint32_t requested, uint32_t &count, uint32_t timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock lock(mutex_);
    count = 0;
    while (count < requested) {
        if (stopped_) throw Error(stopped_, reason_);
        while (!frames_.empty() && count < requested) {
            const auto frame = frames_.front(); frames_.pop_front();
            auto &message = messages[count++];
            message = {};
            message.ProtocolID = CAN; message.RxStatus = frame.status; message.Timestamp = frame.timestamp;
            message.DataSize = frame.size; message.ExtraDataIndex = frame.size;
            std::copy_n(frame.data.begin(), frame.size, message.Data);
        }
        if (count == requested || !timeout_ms || overflow_ || malformed_) break;
        if (!ready_.wait_until(lock, deadline, [&] { return stopped_ || !frames_.empty() || overflow_ || malformed_; })) break;
    }
    if (std::exchange(overflow_, false)) throw Error(ERR_BUFFER_OVERFLOW, "CAN receive buffer overflow; messages lost");
    if (std::exchange(malformed_, false)) throw Error(ERR_INVALID_MSG, "malformed CAN receive frame discarded");
    // Matches the vendor ReadMsgs wrapper: empty even after a timed wait is
    // BUFFER_EMPTY; a nonempty partial timed read is TIMEOUT, retaining count.
    if (!count) throw Error(ERR_BUFFER_EMPTY, "zero CAN messages received");
    if (timeout_ms && count < requested) throw Error(ERR_TIMEOUT, "CAN read returned fewer messages than requested");
}
}
