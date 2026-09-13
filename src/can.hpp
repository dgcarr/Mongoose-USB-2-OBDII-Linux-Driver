#pragma once
#include "transport.hpp"
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
namespace mongoose {
// Compact raw-CAN storage; a full PASSTHRU_MSG is over 4 KB per frame.
class CanReceiver {
public:
    static constexpr size_t capacity = 4096;
    void receive(std::span<const uint8_t> body);
    void stop(int32_t code, const std::string &reason);
    void read(PASSTHRU_MSG *messages, uint32_t requested, uint32_t &count, uint32_t timeout_ms);
private:
    struct Frame {
        uint32_t status, timestamp;
        uint16_t size;
        std::array<uint8_t, 12> data{};
    };
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Frame> frames_;
    bool overflow_ = false, malformed_ = false;
    int32_t stopped_ = 0;
    std::string reason_;
};
Bytes can_pass_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern, uint32_t channel_flags);
}
