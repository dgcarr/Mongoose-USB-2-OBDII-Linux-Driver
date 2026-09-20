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
    // iMsgTxDone accounting. The adapter emits one per transmitted frame, so a blocking WriteMsgs can
    // wait on the count to report what was sent rather than what was merely queued. Only confirmations
    // that pair with a write recorded by note_transmit are counted.
    struct TransmitCount { size_t confirmed = 0; };
    using TransmitCall = std::shared_ptr<TransmitCount>;
    size_t transmitted();
    size_t confirmed(const TransmitCall &call);
    bool await_transmitted(const TransmitCall &call, size_t target, std::chrono::steady_clock::time_point deadline);
    // ISO15765 transmit indication. The vendor DLL puts one message in the receive queue for
    // every request the adapter reports sent: RxStatus TX_MSG_TYPE|TX_DONE, the request's
    // CAN ID alone as data, its TxFlags, and the adapter's own timestamp (Windows captures
    // E1 and E2). The indication carries only the timestamp, so the request is recorded
    // here before it is sent and paired with the indication by sequence number. Raw CAN
    // delivers nothing of the kind unless LOOPBACK is set.
    // Record a request under the sequence its cOutboundData will carry. The vendor pairs by sequence
    // and drops older unpaired records ("loopbackBuf head ... didn't match"), so a frame the bus never
    // confirmed cannot shift later confirmations onto the wrong request. Raw CAN records too: it
    // delivers nothing unless LOOPBACK is set, and that can change between the write and the
    // confirmation. The oldest record is dropped when 256 are outstanding.
    void note_transmit(const PASSTHRU_MSG &message, uint16_t sequence, const TransmitCall &call = {});
    void forget_transmit();  // the write that noted this request was refused
    // SConfig LOOPBACK. It lives on the host: the vendor keeps it in the channel object, never sends
    // it to the firmware, and acts on it when a transmit is confirmed. With it set every confirmed
    // message is also delivered back as a received one with TX_MSG_TYPE, carrying the full frame.
    void set_loopback(bool enabled);
    bool loopback();
    // CLEAR_RX_BUFFER: discard every queued frame and message, any partly reassembled ISO15765
    // conversation, and a pending loss report (which described data that no longer exists).
    void flush_receive();
    // CLEAR_TX_BUFFER: the adapter drops what it had not sent, so those requests will never be
    // confirmed; forget them so a later confirmation cannot pair with one of them.
    void forget_transmits();
private:
    bool deliver_confirmed(uint16_t sequence, uint32_t timestamp);  // true if it paired with a recorded write
    struct Frame {
        uint32_t status, timestamp;
        uint16_t size;
        std::array<uint8_t, 12> data{};
        uint32_t tx_flags = 0;
    };
    struct Message { uint32_t status, timestamp; Bytes data; uint32_t tx_flags = 0; };
    struct PendingTransmit { uint16_t sequence; uint32_t flags; Bytes message; TransmitCall call; };  // message: ID plus data
    const uint16_t protocol_;
    IsoTpReassembler reassembler_;
    std::deque<Message> messages_;
    std::deque<PendingTransmit> pending_transmits_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Frame> frames_;
    size_t transmitted_ = 0;
    bool overflow_ = false, malformed_ = false, loopback_ = false;
    int32_t stopped_ = 0;
    std::string reason_;
};
Bytes can_pass_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern, uint32_t channel_flags);
// BLOCK_FILTER: the same body as a pass filter with table selector 1 and type byte 2, as the vendor
// builder 1000e600 selects for its second filter kind. The pairing comes from that decompile; no
// capture of a block filter existed until the Linux run recorded in docs/VALIDATION.md.
Bytes can_block_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern, uint32_t channel_flags);
// Wire table selectors: where a filter lives, and so which selector removes it.
constexpr uint8_t table_pass = 0, table_block = 1, table_flow_control = 2, table_periodic = 4;
// cOutboundData payload for one raw CAN frame; see PROTOCOL.md section 3 and
// docs/WINDOWS-FINDINGS.md section D. timeout_ms is what the adapter is told, which is
// not the same as how long the host waits for the command response.
Bytes can_transmit(const PASSTHRU_MSG &message, uint32_t timeout_ms);
// cTableAddEntry payload for one periodic CAN message (table selector 4). Vendor sender 1000d220: u32
// table selector, u32 interval in ms, u32 TxFlags, u8 size (ID plus data), then the ID and data as for a
// transmit. J2534 allows 5..65535 ms; the message is checked as a transmit is.
Bytes can_periodic(const PASSTHRU_MSG &message, uint32_t interval_ms);
// ISO15765 flow-control filter, cTableAddEntry with table selector 2. Body layout from
// the Windows D3 capture: the adapter matches the response ID and answers with flow
// control to the request ID; it takes no mask, so a mask that does not cover the whole
// 11-bit identifier is refused rather than silently widened.
Bytes isotp_flow_control_filter(const PASSTHRU_MSG &mask, const PASSTHRU_MSG &pattern,
                                const PASSTHRU_MSG &flow);
// cOutboundData payload for one ISO15765 request. The message carries the CAN ID and
// the service bytes only; the adapter adds the ISO-TP PCI byte (Windows capture E2) and
// segments a message of up to 4095 bytes itself.
Bytes isotp_transmit(const PASSTHRU_MSG &message, uint32_t timeout_ms);
}
