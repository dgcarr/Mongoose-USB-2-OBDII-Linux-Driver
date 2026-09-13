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
    // Count of iMsgTxDone indications. J2534 has nowhere to report these, but whether the
    // adapter confirms a transmit is exactly what a bench run with no bus needs to know.
    size_t transmitted();
private:
    struct Frame {
        uint32_t status, timestamp;
        uint16_t size;
        std::array<uint8_t, 12> data{};
    };
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Frame> frames_;
    size_t transmitted_ = 0;
    bool overflow_ = false, malformed_ = false;
    int32_t stopped_ = 0;
    std::string reason_;
};
Bytes can_pass_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern, uint32_t channel_flags);
// cOutboundData payload for one raw CAN frame; see PROTOCOL.md section 3 and
// docs/WINDOWS-FINDINGS.md section D. timeout_ms is what the adapter is told, which is
// not the same as how long the host waits for the command response.
Bytes can_transmit(const PASSTHRU_MSG &message, uint32_t channel_flags, uint32_t timeout_ms);
}
