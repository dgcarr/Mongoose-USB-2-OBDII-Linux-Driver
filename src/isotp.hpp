#pragma once
#include "codec.hpp"
#include "mongoose/j2534.h"
#include <span>
namespace mongoose {
// Host-side ISO15765 receive reassembly.
//
// The split is settled by capture (docs/WINDOWS-FINDINGS.md section D/E): the adapter
// generates flow control by itself -- it reports the FC frame as already transmitted and
// the host never sends one -- while the DLL reassembles multi-frame responses. So this
// is a pure function of the inbound frame sequence, with no timing loop and no wire
// traffic, which is why it can be finished and proven correct with no vehicle.
//
// Modelled on the vendor's own dispatch: 10021790 routes by PCI type, 10020fc0 handles
// single frames, 10021070 first frames, 10021280 consecutive frames.
//
// Standard addressing only. The vendor shifts every offset by one byte for extended
// addressing; that path is deliberately absent because no capture exercises it and
// untested reassembly is worse than none.
class IsoTpReassembler {
public:
    // The vendor's own bound on a reassembled message (10021070 rejects >= 0x1000).
    static constexpr size_t max_message = 0xfff;
    struct Output {
        // 0 for a completed message, START_OF_MESSAGE for the first-frame indication.
        uint32_t rx_status = 0;
        // Four big-endian CAN ID bytes, then the reassembled payload. An indication
        // carries the ID alone, as the vendor's zero-length start-of-message does.
        Bytes data;
    };
    // One cInboundData payload: four ID bytes followed by the CAN data bytes.
    std::vector<Output> feed(std::span<const uint8_t> frame);
    void reset();
    bool assembling() const { return assembling_; }
private:
    std::vector<Output> single(std::span<const uint8_t> frame);
    std::vector<Output> first(std::span<const uint8_t> frame);
    std::vector<Output> consecutive(std::span<const uint8_t> frame);
    Bytes message_;          // identifier plus the payload accumulated so far
    size_t expected_ = 0;    // total payload length announced by the first frame
    uint8_t sequence_ = 0;   // next consecutive-frame sequence, low nibble only
    bool assembling_ = false;
};
}
