#include "isotp.hpp"
#include <algorithm>
namespace mongoose {
namespace {
constexpr size_t identifier = 4;       // big-endian CAN ID ahead of every payload
constexpr size_t single_limit = 7;     // 10020fc0 rejects a longer single frame
constexpr size_t segmented_minimum = 8;// 10021070 rejects a first frame that would fit in one
Bytes with_identifier(std::span<const uint8_t> frame) {
    return Bytes(frame.begin(), frame.begin() + identifier);
}
}
void IsoTpReassembler::reset() {
    message_.clear(); expected_ = 0; sequence_ = 0; assembling_ = false;
}
std::vector<IsoTpReassembler::Output> IsoTpReassembler::feed(std::span<const uint8_t> frame) {
    // 10021790 forces an unusable type when the frame is too short to hold a PCI byte,
    // which lands in the ignore branch.
    if (frame.size() <= identifier) return {};
    const auto type = static_cast<uint8_t>(frame[identifier] >> 4);
    if (!assembling_) {
        if (type == 0) return single(frame);
        if (type == 1) return first(frame);
        return {};  // a consecutive or flow-control frame with nothing in progress
    }
    switch (type) {
    case 0:
        // "SingleFrame arrived, interrupted segmented receive": the vendor delivers the
        // single frame and leaves the partial message in place, so a later consecutive
        // frame still continues it. Reproduced deliberately, odd as it looks.
        return single(frame);
    case 1:
        // "FirstFrame arrived, interrupted segmented receive": 10021440 discards the
        // partial message before 10021070 starts the new one.
        reset();
        return first(frame);
    case 2:
        return consecutive(frame);
    default:
        return {};
    }
}
std::vector<IsoTpReassembler::Output> IsoTpReassembler::single(std::span<const uint8_t> frame) {
    if (frame.size() < identifier + 1) { reset(); return {}; }
    const size_t length = frame[identifier] & 0x0f;
    if (length > single_limit) { reset(); return {}; }
    // The vendor copies the announced length without checking the frame actually carries
    // it, which reads past a short frame. We refuse instead: a single frame claiming
    // more data than it holds is malformed, and delivering the difference would mean
    // handing the caller whatever followed in memory.
    if (length > frame.size() - identifier - 1) { reset(); return {}; }
    Output message{0, with_identifier(frame)};
    const auto payload = frame.subspan(identifier + 1, length);
    message.data.insert(message.data.end(), payload.begin(), payload.end());
    return {std::move(message)};
}
std::vector<IsoTpReassembler::Output> IsoTpReassembler::first(std::span<const uint8_t> frame) {
    if (frame.size() < identifier + 2) return {};
    const size_t announced = static_cast<size_t>(frame[identifier] & 0x0f) << 8 | frame[identifier + 1];
    // A message short enough for a single frame must not be segmented. The upper bound
    // mirrors 10021070's `< 0x1000` and cannot actually fail here, since a twelve-bit
    // field cannot exceed 0xfff; it stays so the limit is stated where the parse is.
    if (announced < segmented_minimum || announced > max_message) return {};
    message_ = with_identifier(frame);
    const auto payload = frame.subspan(identifier + 2);
    const auto taken = std::min(payload.size(), announced);
    message_.insert(message_.end(), payload.begin(), payload.begin() + static_cast<ptrdiff_t>(taken));
    expected_ = announced;
    sequence_ = 1;
    assembling_ = true;
    // The vendor raises a zero-length start-of-message here; the Windows capture shows
    // it reaching the API as RxStatus 2 ahead of the reassembled message.
    return {Output{START_OF_MESSAGE, with_identifier(frame)}};
}
std::vector<IsoTpReassembler::Output> IsoTpReassembler::consecutive(std::span<const uint8_t> frame) {
    if (frame.size() < identifier + 2) { reset(); return {}; }
    if ((frame[identifier] & 0x0f) != sequence_) {
        // "ISO15765 SequenceNum got %d expected %d, killing receive": a gap means the
        // rest of the message can never be trusted, so the whole thing is discarded.
        reset();
        return {};
    }
    sequence_ = static_cast<uint8_t>((sequence_ + 1) & 0x0f);
    const auto payload = frame.subspan(identifier + 1);
    const auto outstanding = expected_ - (message_.size() - identifier);
    const auto taken = std::min(payload.size(), outstanding);
    message_.insert(message_.end(), payload.begin(), payload.begin() + static_cast<ptrdiff_t>(taken));
    if (message_.size() - identifier < expected_) return {};
    Output message{0, std::move(message_)};
    reset();
    return {std::move(message)};
}
}
