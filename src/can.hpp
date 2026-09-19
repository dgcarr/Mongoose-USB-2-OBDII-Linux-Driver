#pragma once
#include "isotp.hpp"
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
    // ISO15765 messages are reassembled here, so each can be up to 4 KB; the backlog is
    // bounded far lower than the raw-frame queue for that reason.
    static constexpr size_t message_capacity = 256;
    // CAN keeps raw frames. ISO15765 runs them through IsoTpReassembler and queues whole
    // messages, because the adapter delivers segments with the PCI bytes intact.
    explicit CanReceiver(uint16_t protocol = CAN) : protocol_(protocol) {}
    void receive(std::span<const uint8_t> body);
    void stop(int32_t code, const std::string &reason);
    void read(PASSTHRU_MSG *messages, uint32_t requested, uint32_t &count, uint32_t timeout_ms);
    // iMsgTxDone accounting. The adapter emits exactly one per transmitted frame, so a
    // blocking WriteMsgs can wait on the count to report what was sent rather than what
    // was merely queued.
    size_t transmitted();
    bool await_transmitted(size_t target, std::chrono::steady_clock::time_point deadline);
private:
    struct Frame {
        uint32_t status, timestamp;
        uint16_t size;
        std::array<uint8_t, 12> data{};
    };
    struct Message { uint32_t status, timestamp; Bytes data; };
    const uint16_t protocol_;
    IsoTpReassembler reassembler_;
    std::deque<Message> messages_;
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
// ISO15765 flow-control filter, cTableAddEntry with table selector 2. Body layout from
// the Windows D3 capture: the adapter matches the response ID and answers with flow
// control to the request ID; it takes no mask, so a mask that does not cover the whole
// 11-bit identifier is refused rather than silently widened.
Bytes isotp_flow_control_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern,
                                const PASSTHRU_MSG &flow);
// cOutboundData payload for one ISO15765 request. The message carries the CAN ID and
// the service bytes only; the adapter adds the ISO-TP PCI byte (Windows capture E2).
// Single-frame requests only: nothing here has exercised a segmented transmit.
Bytes isotp_transmit(const PASSTHRU_MSG &message, uint32_t timeout_ms);
}
