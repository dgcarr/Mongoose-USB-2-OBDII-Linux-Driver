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
// One conversation is tracked per source CAN ID. A functional request is answered by several
// ECUs at once, and their multi-frame replies interleave on the bus; a single shared assembly
// would let one ECU's first frame discard another's partial message (seen on the live car with
// mode 09 PIDs 04 and 0A). The vendor rules below apply within one ID, unchanged.
//
// Standard addressing only. The vendor shifts every offset by one byte for extended
// addressing (analysis/decompiled/isotp/10021070.c branches on an addressing flag in its
// state block); that path is deliberately absent because no live capture, Linux or Windows,
// exercises it, and untested reassembly is worse than none. ISO15765_ADDR_TYPE exists as a
// ConnectFlags bit for this but is rejected as an unsupported flag until a real capture
// exists to build a fixture from, as tests/fixtures/isotp-vin.* came from one.
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
    // Concurrent conversations kept. When a new first frame arrives with all slots in use the
    // oldest is discarded, so stale partial messages cannot pin memory or starve later ones.
    static constexpr size_t max_conversations = 32;
    // One cInboundData payload: four ID bytes followed by the CAN data bytes.
    std::vector<Output> feed(std::span<const uint8_t> frame);
    void reset();
    bool assembling() const { return !assemblies_.empty(); }
    bool assembling(uint32_t id) const;
private:
    struct Assembly {
        uint32_t id;
        Bytes message;         // identifier plus the payload accumulated so far
        size_t expected;       // total payload length announced by the first frame
        uint8_t sequence;      // next consecutive-frame sequence, low nibble only
    };
    std::vector<Output> single(std::span<const uint8_t> frame);
    std::vector<Output> first(std::span<const uint8_t> frame);
    std::vector<Output> consecutive(std::span<const uint8_t> frame);
    std::vector<Assembly>::iterator find(uint32_t id);
    void drop(uint32_t id);
    std::vector<Assembly> assemblies_;  // oldest first
};
}
