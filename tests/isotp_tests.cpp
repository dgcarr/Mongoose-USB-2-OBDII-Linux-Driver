// Host-side ISO15765 reassembly, tested against the captured VIN exchange. No hardware
// is involved: the adapter generates flow control itself, so reassembly is a pure
// function of the inbound frame sequence.
#include "can.hpp"
#include "isotp.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace mongoose;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at " + std::to_string(__LINE__)); } while (0)
namespace {
std::vector<std::string> fixture_lines(const char *name) {
    std::ifstream file(std::string(FIXTURE_DIR) + "/" + name);
    CHECK(file.good());
    std::vector<std::string> lines;
    for (std::string line; std::getline(file, line);)
        if (!line.empty() && line[0] != '#') lines.push_back(line);
    return lines;
}
std::string describe(const IsoTpReassembler::Output &output) {
    std::ostringstream text;
    text << "status=" << output.rx_status << " data=" << hex(output.data);
    return text.str();
}
// The whole point of the exercise: three frames on the wire, one message to the caller.
void captured_vin() {
    IsoTpReassembler reassembler;
    std::vector<std::string> produced;
    for (const auto &line : fixture_lines("isotp-vin.frames"))
        for (const auto &output : reassembler.feed(unhex(line))) produced.push_back(describe(output));
    const auto expected = fixture_lines("isotp-vin.expected");
    CHECK(produced.size() == expected.size());
    for (size_t i = 0; i < produced.size(); ++i) CHECK(produced[i] == expected[i]);
    CHECK(!reassembler.assembling());
    // 4 identifier bytes plus the 20 announced by the first frame, as the vendor API
    // reported for this exchange.
    CHECK(unhex(expected.back().substr(expected.back().find("data=") + 5)).size() == 24);
}
void single_frame() {
    IsoTpReassembler reassembler;
    auto out = reassembler.feed(unhex("000007e80341024f"));  // 3 bytes: 41 02 4f
    CHECK(out.size() == 1 && out[0].rx_status == 0);
    CHECK(hex(out[0].data) == "000007e841024f");
    // A single frame may carry at most seven bytes, and may not claim more than it holds.
    CHECK(reassembler.feed(unhex("000007e808010203040506070809")).empty());
    CHECK(reassembler.feed(unhex("000007e80741")).empty());
}
void sequence_gap_kills_receive() {
    IsoTpReassembler reassembler;
    CHECK(reassembler.feed(unhex("000007e81014490201524544")).size() == 1);
    CHECK(reassembler.assembling());
    // Sequence 2 arrives where 1 was expected: the partial message is discarded rather
    // than silently stitched together with a hole in it.
    CHECK(reassembler.feed(unhex("000007e82241435445445649")).empty());
    CHECK(!reassembler.assembling());
    // The frame that would have completed it now finds nothing in progress.
    CHECK(reassembler.feed(unhex("000007e8224e303030303030")).empty());
}
void first_frame_restarts_assembly() {
    IsoTpReassembler reassembler;
    CHECK(reassembler.feed(unhex("000007e81014490201524544")).size() == 1);
    auto restarted = reassembler.feed(unhex("000007e81008aabbccddeeff"));
    CHECK(restarted.size() == 1 && restarted[0].rx_status == START_OF_MESSAGE);
    // The new message is eight bytes: six from the first frame, two more to come.
    auto done = reassembler.feed(unhex("000007e8211122"));
    CHECK(done.size() == 1 && done[0].rx_status == 0);
    CHECK(hex(done[0].data) == "000007e8aabbccddeeff1122");
}
void single_frame_does_not_disturb_assembly() {
    IsoTpReassembler reassembler;
    CHECK(reassembler.feed(unhex("000007e81014490201524544")).size() == 1);
    // The vendor logs this as an interruption, delivers the single frame and leaves the
    // partial message in place. Reproduced on purpose, so the consecutive frames that
    // follow still complete the original message.
    auto interruption = reassembler.feed(unhex("000007e80341024f"));
    CHECK(interruption.size() == 1 && hex(interruption[0].data) == "000007e841024f");
    CHECK(reassembler.assembling());
    CHECK(reassembler.feed(unhex("000007e82141435445445649")).empty());
    auto done = reassembler.feed(unhex("000007e8224e303030303030"));
    CHECK(done.size() == 1);
    CHECK(hex(done[0].data) == "000007e8490201524544414354454456494e303030303030");
}
void rejects_malformed_first_frames() {
    IsoTpReassembler reassembler;
    // A message that fits in a single frame must not be segmented.
    CHECK(reassembler.feed(unhex("000007e81007010203040506")).empty());
    CHECK(!reassembler.assembling());
    // 0xfff is the largest a twelve-bit length field can express and the vendor accepts
    // it, so this starts a (very long) assembly rather than being rejected.
    CHECK(reassembler.feed(unhex("000007e81fff010203040506")).size() == 1);
    CHECK(reassembler.assembling());
    reassembler.reset();
    // Too short to carry a length byte at all.
    CHECK(reassembler.feed(unhex("000007e810")).empty());
    // Nothing in progress, so consecutive and flow-control frames are ignored.
    CHECK(reassembler.feed(unhex("000007e82141435445445649")).empty());
    CHECK(reassembler.feed(unhex("000007e8300000")).empty());
    CHECK(reassembler.feed(unhex("000007e8")).empty());
}
void foreign_can_id_does_not_disturb_assembly() {
    IsoTpReassembler reassembler;
    CHECK(reassembler.feed(unhex("000007e81014490201524544")).size() == 1);
    CHECK(reassembler.assembling());
    // Consecutive frame with foreign CAN ID (000007e9 vs 000007e8) must be ignored,
    // and must not kill or corrupt the ongoing reassembly on 000007e8.
    CHECK(reassembler.feed(unhex("000007e92141435445445649")).empty());
    CHECK(reassembler.assembling());
    // Also a consecutive frame with foreign ID and wrong sequence must not kill it.
    CHECK(reassembler.feed(unhex("000007e92541435445445649")).empty());
    CHECK(reassembler.assembling());
    // The correct consecutive frames still complete the original message.
    CHECK(reassembler.feed(unhex("000007e82141435445445649")).empty());
    auto done = reassembler.feed(unhex("000007e8224e303030303030"));
    CHECK(done.size() == 1);
    CHECK(hex(done[0].data) == "000007e8490201524544414354454456494e303030303030");
    CHECK(!reassembler.assembling());
}
void trailing_bytes_are_trimmed() {
    IsoTpReassembler reassembler;
    // Announced length 8: six bytes arrive first, then a padded consecutive frame whose
    // seven data bytes overrun by five. The message must stop at the announced length.
    CHECK(reassembler.feed(unhex("000007e81008aabbccddeeff")).size() == 1);
    auto done = reassembler.feed(unhex("000007e82111225555555555"));
    CHECK(done.size() == 1);
    CHECK(hex(done[0].data) == "000007e8aabbccddeeff1122");
    CHECK(!reassembler.assembling());
}
PASSTHRU_MSG iso_message(uint32_t flags, const char *hex_data) {
    PASSTHRU_MSG message{};
    const auto bytes = unhex(hex_data);
    message.ProtocolID = ISO15765; message.TxFlags = flags; message.DataSize = static_cast<uint32_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), message.Data);
    return message;
}
template<class F> void refused(F &&operation, int32_t code) {
    try { operation(); } catch (const Error &error) { CHECK(error.code == code); return; }
    throw std::runtime_error("expected an Error");
}
// Byte for byte the cTableAddEntry body of Windows capture D3/E2 (selector 2, FRAME_PAD,
// response ID 0x7E8, flow-control ID 0x7E0). The mask is the one the vendor's own step used.
void vendor_flow_control_filter() {
    const auto mask = iso_message(ISO15765_FRAME_PAD, "0000ffff");
    const auto pattern = iso_message(ISO15765_FRAME_PAD, "000007e8");
    const auto flow = iso_message(ISO15765_FRAME_PAD, "000007e0");
    CHECK(hex(isotp_flow_control_filter(mask, pattern, flow)) == "020000004000000000000007e800000007e000");
    // The adapter matches the whole identifier, so a mask that does not cover it is refused.
    refused([&] { isotp_flow_control_filter(iso_message(ISO15765_FRAME_PAD, "000000ff"), pattern, flow); }, ERR_INVALID_MSG);
    refused([&] { isotp_flow_control_filter(mask, iso_message(0, "000007e8"), flow); }, ERR_INVALID_MSG);
    refused([&] { isotp_flow_control_filter(mask, iso_message(ISO15765_FRAME_PAD, "000107e8"), flow); }, ERR_INVALID_MSG);
    auto wrong = pattern; wrong.ProtocolID = CAN;
    refused([&] { isotp_flow_control_filter(mask, wrong, flow); }, ERR_MSG_PROTOCOL_ID);
}
// Windows capture E2: the request is the CAN ID and the service bytes, with no PCI byte.
void vendor_transmit() {
    CHECK(hex(isotp_transmit(iso_message(ISO15765_FRAME_PAD, "000007df0902"), 1000)) ==
          "40000000e803000006000000000007df0902");
    refused([&] { isotp_transmit(iso_message(ISO15765_FRAME_PAD, "000007df"), 1000); }, ERR_INVALID_MSG);
    refused([&] { isotp_transmit(iso_message(ISO15765_ADDR_TYPE, "000007df0902"), 1000); }, ERR_INVALID_FLAGS);
    // Longer than one frame: the whole message goes to the adapter in one command, unchanged, as the
    // vendor's frame builder does; the adapter segments it.
    CHECK(hex(isotp_transmit(iso_message(0, "000007df0102030405060708"), 1000)) ==
          "00000000e8030000" "0c000000" "000007df0102030405060708");
    Bytes longest(4 + 4095, 0xaa); longest[0] = longest[1] = 0; longest[2] = 0x07; longest[3] = 0xe0;
    PASSTHRU_MSG maximal{}; maximal.ProtocolID = ISO15765; maximal.DataSize = 4 + 4095;
    std::copy(longest.begin(), longest.end(), maximal.Data);
    CHECK(isotp_transmit(maximal, 1000).size() == 12 + 4 + 4095);
    maximal.DataSize = 4 + 4096;
    refused([&] { isotp_transmit(maximal, 1000); }, ERR_INVALID_MSG);
}
Bytes inbound(uint32_t timestamp, const char *frame) {
    Bytes body(24, 0);
    put16(body, 2, channel_node(ISO15765)); put16(body, 4, 9);
    put16(body, 16, static_cast<uint16_t>(timestamp)); put16(body, 22, 12);
    const auto data = unhex(frame);
    body.insert(body.end(), data.begin(), data.end());
    return body;
}
// The three captured VIN frames, through the receiver the ISO15765 channel actually uses.
void receiver_reassembles_captured_vin() {
    CanReceiver receiver(ISO15765);
    for (const char *frame : {"000007e81014490201524544", "000007e82141435445445649", "000007e8224e303030303030"})
        receiver.receive(inbound(1, frame));
    PASSTHRU_MSG out[3]{}; uint32_t count = 0;
    try { receiver.read(out, 3, count, 0); } catch (const Error &error) { CHECK(error.code == ERR_TIMEOUT || error.code == ERR_BUFFER_EMPTY); }
    CHECK(count == 2);
    CHECK(out[0].ProtocolID == ISO15765 && out[0].RxStatus == START_OF_MESSAGE && out[0].DataSize == 4);
    CHECK(out[1].ProtocolID == ISO15765 && out[1].RxStatus == 0 && out[1].DataSize == 24);
    CHECK(hex(Bytes(out[1].Data, out[1].Data + 4)) == "000007e8");
    // A CAN channel never sees the reassembler: a frame for the other protocol is ignored.
    CanReceiver can(CAN);
    can.receive(inbound(1, "000007e81014490201524544"));
    PASSTHRU_MSG none{}; uint32_t got = 0;
    try { can.read(&none, 1, got, 0); } catch (const Error &error) { CHECK(error.code == ERR_BUFFER_EMPTY); }
    CHECK(got == 0);
}
}
Bytes tx_done(uint32_t timestamp, uint16_t sequence, uint16_t node = channel_node(ISO15765)) {
    // cIndication 0x106 (iMsgTxDone): the confirmed command's sequence echoed at +6, chan 1 at +8,
    // the code at +12 and a timestamp, as captured (Windows E1: seq 11).
    Bytes body(20, 0);
    put16(body, 2, node); put16(body, 4, 10); put16(body, 6, sequence); put16(body, 8, 1); put16(body, 12, 0x106);
    put16(body, 16, static_cast<uint16_t>(timestamp)); put16(body, 18, static_cast<uint16_t>(timestamp >> 16));
    return body;
}
PASSTHRU_MSG request_to(const char *id, uint32_t flags) {
    PASSTHRU_MSG message{};
    message.ProtocolID = ISO15765; message.TxFlags = flags;
    const auto bytes = unhex(id);
    std::copy(bytes.begin(), bytes.end(), message.Data); message.DataSize = static_cast<uint32_t>(bytes.size());
    return message;
}
// Windows captures E1/E2: every ISO15765 request puts a message ahead of the reply, RxStatus
// TX_MSG_TYPE|TX_DONE, the request ID alone, TxFlags 0x40, ExtraDataIndex 0 and the adapter's
// timestamp. The start-of-message indication that follows also reports extra=0.
void receiver_reports_transmit_done() {
    CanReceiver receiver(ISO15765);
    receiver.note_transmit(request_to("000007df0902", ISO15765_FRAME_PAD), 11);
    receiver.receive(tx_done(6031900, 11));
    receiver.receive(inbound(6037100, "000007e81014490201524544"));
    CHECK(receiver.transmitted() == 1);
    PASSTHRU_MSG out[4]{}; uint32_t count = 0;
    try { receiver.read(out, 4, count, 0); } catch (const Error &error) { CHECK(error.code == ERR_TIMEOUT || error.code == ERR_BUFFER_EMPTY); }
    CHECK(count == 2);
    CHECK(out[0].ProtocolID == ISO15765 && out[0].RxStatus == (TX_MSG_TYPE | TX_DONE) && out[0].RxStatus == 9);
    CHECK(out[0].TxFlags == ISO15765_FRAME_PAD && out[0].Timestamp == 6031900);
    CHECK(out[0].DataSize == 4 && out[0].ExtraDataIndex == 0);
    CHECK(hex(Bytes(out[0].Data, out[0].Data + 4)) == "000007df");
    CHECK(out[1].RxStatus == START_OF_MESSAGE && out[1].DataSize == 4 && out[1].ExtraDataIndex == 0);
    CHECK(out[1].TxFlags == 0);

    // Confirmations pair with requests by sequence, and a refused write leaves none behind.
    CanReceiver ordered(ISO15765);
    ordered.note_transmit(request_to("000007df0100", 0), 5);
    ordered.note_transmit(request_to("000007e00902", ISO15765_FRAME_PAD), 6);
    ordered.note_transmit(request_to("000007e10902", 0), 7);
    ordered.forget_transmit();
    ordered.receive(tx_done(10, 5)); ordered.receive(tx_done(20, 6));
    PASSTHRU_MSG pair[3]{}; count = 0;
    try { ordered.read(pair, 3, count, 0); } catch (const Error &error) { CHECK(error.code == ERR_TIMEOUT || error.code == ERR_BUFFER_EMPTY); }
    CHECK(count == 2);
    CHECK(hex(Bytes(pair[0].Data, pair[0].Data + 4)) == "000007df" && pair[0].Timestamp == 10 && pair[0].TxFlags == 0);
    CHECK(hex(Bytes(pair[1].Data, pair[1].Data + 4)) == "000007e0" && pair[1].Timestamp == 20 && pair[1].TxFlags == ISO15765_FRAME_PAD);

    // A confirmation with no recorded request (a running periodic message, say) queues nothing and is not
    // counted, so it cannot be mistaken for a timed write finishing; one for another node is ignored.
    CanReceiver stray(ISO15765);
    stray.receive(tx_done(5, 40));
    stray.receive(tx_done(6, 41, channel_node(CAN)));
    CHECK(stray.transmitted() == 0);
    PASSTHRU_MSG none{}; count = 0;
    try { stray.read(&none, 1, count, 0); CHECK(false); } catch (const Error &error) { CHECK(error.code == ERR_BUFFER_EMPTY); }

    // Raw CAN counts the confirmation and delivers no message for it.
    CanReceiver can(CAN);
    can.receive(tx_done(7, 42, channel_node(CAN)));
    CHECK(can.transmitted() == 0);                 // unpaired: not counted
    PASSTHRU_MSG raw_frame{}; raw_frame.ProtocolID = CAN; raw_frame.DataSize = 6;
    can.note_transmit(raw_frame, 43);
    can.receive(tx_done(8, 43, channel_node(CAN)));
    CHECK(can.transmitted() == 1);                 // paired: counted, and still no message without loopback
    count = 0;
    try { can.read(&none, 1, count, 0); CHECK(false); } catch (const Error &error) { CHECK(error.code == ERR_BUFFER_EMPTY); }

    // stop() drops requests that will never be confirmed, so a later confirmation cannot pair with them.
    CanReceiver stopped(ISO15765);
    stopped.note_transmit(request_to("000007df0100", 0), 9);
    stopped.stop(ERR_DEVICE_NOT_CONNECTED, "unplugged");
    CHECK(stopped.transmitted() == 0);
}
// Live car, script h2: a physical request an ECU cannot serve draws a negative response, which
// arrives as an ordinary single-frame message (7F, service, code) with no special status. The
// reassembled messages below are as captured (analysis/captures/linux-script-h2-*.txt); the
// CAN-level padding after the PCI-announced bytes was not captured, so zeros stand in for it.
void negative_responses_are_ordinary_messages() {
    IsoTpReassembler reassembler;
    auto out = reassembler.feed(unhex("000007e8037f091200000000"));
    CHECK(out.size() == 1 && out[0].rx_status == 0 && hex(out[0].data) == "000007e87f0912");
    out = reassembler.feed(unhex("000007e8037f223100000000"));
    CHECK(out.size() == 1 && out[0].rx_status == 0 && hex(out[0].data) == "000007e87f2231");
    // Read through a receiver: the size the caller sees is the announced length plus the ID.
    CanReceiver receiver(ISO15765);
    receiver.receive(inbound(2515100, "000007e8037f223100000000"));
    PASSTHRU_MSG message{}; uint32_t count = 0;
    receiver.read(&message, 1, count, 0);
    CHECK(count == 1 && message.DataSize == 7 && message.ExtraDataIndex == 7 && message.RxStatus == 0);
    CHECK(message.Data[4] == 0x7f && message.Data[5] == 0x22 && message.Data[6] == 0x31);
    // A request nobody answers leaves only the transmit indication: a partial read.
    CanReceiver silent(ISO15765);
    silent.note_transmit(request_to("000007df017f", ISO15765_FRAME_PAD), 3);
    silent.receive(tx_done(605100, 3));
    count = 0;
    try { silent.read(&message, 16, count, 1); CHECK(false); } catch (const Error &error) { CHECK(error.code == ERR_TIMEOUT); }
    CHECK(count == 1 && message.RxStatus == 9);
    // And with no transmit indication either, the read is simply empty.
    CanReceiver quiet(ISO15765);
    count = 0;
    try { quiet.read(&message, 16, count, 1); CHECK(false); } catch (const Error &error) { CHECK(error.code == ERR_BUFFER_EMPTY); }
    CHECK(count == 0);
}
// Split a reassembled message (ID plus payload) into the CAN frames an ECU would send: a first
// frame carrying six payload bytes, then consecutive frames of seven, zero-padded to eight data
// bytes. The messages themselves are captured; the frame split is the ISO 15765-2 rule.
std::vector<Bytes> segment(const std::string &message_hex) {
    const auto message = unhex(message_hex);
    const size_t length = message.size() - 4;
    std::vector<Bytes> frames;
    Bytes frame(message.begin(), message.begin() + 4);
    frame.push_back(static_cast<uint8_t>(0x10 | (length >> 8))); frame.push_back(static_cast<uint8_t>(length));
    size_t at = 4;
    for (size_t i = 0; i < 6 && at < message.size(); ++i) frame.push_back(message[at++]);
    frame.resize(12, 0); frames.push_back(frame);
    for (uint8_t sequence = 1; at < message.size(); sequence = static_cast<uint8_t>((sequence + 1) & 0x0f)) {
        Bytes next(message.begin(), message.begin() + 4);
        next.push_back(static_cast<uint8_t>(0x20 | sequence));
        for (size_t i = 0; i < 7 && at < message.size(); ++i) next.push_back(message[at++]);
        next.resize(12, 0); frames.push_back(next);
    }
    return frames;
}
// Live car, script h3: a functional mode 09 request is answered by both ECUs at once, and their
// multi-frame replies interleave on the bus. With one shared assembly the second ECU's first
// frame discarded the first ECU's partial message: two start-of-message indications arrived and
// only one message completed, for PID 04 and again for PID 0A. Each ECU now has its own state.
void interleaved_ecu_replies_complete() {
    const std::string calibration = "000007e84904023331363639353530204141000000000033313337323637392041420000000000";
    const std::string ecu_name = "000007e9490a0154434d002d5472616e736d69734374726c000000";
    const auto a = segment(calibration), b = segment(ecu_name);
    CHECK(a.size() == 6 && b.size() == 4);  // 35 payload bytes and 23
    IsoTpReassembler reassembler;
    std::vector<IsoTpReassembler::Output> all;
    for (size_t i = 0; i < a.size(); ++i) {
        for (const auto *frames : {&a, &b}) {
            if (i >= frames->size()) continue;
            for (auto &output : reassembler.feed((*frames)[i])) all.push_back(std::move(output));
        }
        if (i == 0) CHECK(reassembler.assembling(0x7e8) && reassembler.assembling(0x7e9));
    }
    CHECK(!reassembler.assembling());
    // Two indications, then each message once, whichever finishes first.
    CHECK(all.size() == 4);
    CHECK(all[0].rx_status == START_OF_MESSAGE && hex(all[0].data) == "000007e8");
    CHECK(all[1].rx_status == START_OF_MESSAGE && hex(all[1].data) == "000007e9");
    CHECK(all[2].rx_status == 0 && all[3].rx_status == 0);
    const std::string first_done = hex(all[2].data), second_done = hex(all[3].data);
    CHECK((first_done == ecu_name && second_done == calibration) || (first_done == calibration && second_done == ecu_name));

    // Within one ID the vendor rules still hold: a second first frame from the same ECU restarts
    // that ECU's message and leaves the other's alone.
    IsoTpReassembler restart;
    for (const auto &frame : {a[0], b[0], a[1]}) restart.feed(frame);
    CHECK(restart.feed(a[0]).size() == 1);   // new indication for 0x7e8
    for (size_t i = 1; i < a.size(); ++i) {
        auto out = restart.feed(a[i]);
        if (i + 1 == a.size()) CHECK(out.size() == 1 && hex(out[0].data) == calibration);
    }
    for (size_t i = 1; i < b.size(); ++i) {
        auto out = restart.feed(b[i]);
        if (i + 1 == b.size()) CHECK(out.size() == 1 && hex(out[0].data) == ecu_name);
    }
    // A sequence gap kills only the ECU it belongs to.
    IsoTpReassembler gap;
    gap.feed(a[0]); gap.feed(b[0]);
    CHECK(gap.feed(a[2]).empty());            // skipped a[1]
    CHECK(!gap.assembling(0x7e8) && gap.assembling(0x7e9));
    for (size_t i = 1; i < b.size(); ++i) {
        auto out = gap.feed(b[i]);
        if (i + 1 == b.size()) CHECK(out.size() == 1 && hex(out[0].data) == ecu_name);
    }
}
// A bus that starts many conversations and never finishes them cannot grow the reassembler without
// bound, and cannot lock out a real reply: the oldest conversation is the one given up.
void conversation_limit_drops_the_oldest() {
    const auto frames = segment("000007e8490a0154434d002d5472616e736d69734374726c000000");
    IsoTpReassembler reassembler;
    for (uint32_t id = 0x100; id < 0x100 + IsoTpReassembler::max_conversations; ++id) {
        Bytes first = frames[0]; first[2] = static_cast<uint8_t>(id >> 8); first[3] = static_cast<uint8_t>(id);
        CHECK(reassembler.feed(first).size() == 1);
    }
    CHECK(reassembler.assembling(0x100));
    Bytes newest = frames[0]; newest[2] = 0x07; newest[3] = 0xe8;
    CHECK(reassembler.feed(newest).size() == 1);   // one over the limit: 0x100 is dropped
    CHECK(!reassembler.assembling(0x100) && reassembler.assembling(0x101) && reassembler.assembling(0x7e8));
    reassembler.reset();
    CHECK(!reassembler.assembling());
}
// Vendor 1000bcb0 pairs a confirmation with its request by sequence and discards older unpaired
// records; an unknown sequence leaves them alone. A frame the bus never confirmed therefore cannot
// shift later confirmations onto the wrong request.
void transmit_pairing_by_sequence() {
    PASSTHRU_MSG out[4]{}; uint32_t count = 0;
    const auto drain = [&](CanReceiver &receiver, uint32_t wanted) {
        count = 0;
        try { receiver.read(out, wanted, count, 0); } catch (const Error &error) { CHECK(error.code == ERR_TIMEOUT || error.code == ERR_BUFFER_EMPTY); }
    };
    CanReceiver stale(ISO15765);
    stale.note_transmit(request_to("000007df0100", 0), 1);        // never confirmed
    stale.note_transmit(request_to("000007e00902", 0), 2);        // never confirmed
    stale.note_transmit(request_to("000007e10902", ISO15765_FRAME_PAD), 3);
    stale.receive(tx_done(30, 3));
    drain(stale, 4);
    CHECK(count == 1 && hex(Bytes(out[0].Data, out[0].Data + 4)) == "000007e1" && out[0].Timestamp == 30);
    stale.receive(tx_done(40, 2));                                 // its record was discarded as stale
    drain(stale, 4); CHECK(count == 0);
    CHECK(stale.transmitted() == 1);                               // only the one that paired counts

    CanReceiver unknown(ISO15765);
    unknown.note_transmit(request_to("000007df0100", 0), 1);
    unknown.note_transmit(request_to("000007e00902", 0), 2);
    unknown.receive(tx_done(50, 99));                              // no such sequence: nothing touched
    drain(unknown, 4); CHECK(count == 0);
    unknown.receive(tx_done(60, 1));
    drain(unknown, 4); CHECK(count == 1 && hex(Bytes(out[0].Data, out[0].Data + 4)) == "000007df");
    unknown.receive(tx_done(70, 2));
    drain(unknown, 4); CHECK(count == 1 && hex(Bytes(out[0].Data, out[0].Data + 4)) == "000007e0");

    // 256 outstanding: the 257th note drops the oldest instead of failing the write.
    CanReceiver full(ISO15765);
    for (uint16_t sequence = 1; sequence <= CanReceiver::message_capacity + 1; ++sequence)
        full.note_transmit(request_to("000007df0100", 0), sequence);
    full.receive(tx_done(80, 1));                                  // dropped
    drain(full, 4); CHECK(count == 0);
    full.receive(tx_done(90, 2));
    drain(full, 4); CHECK(count == 1);
}
// LOOPBACK is host-side (vendor keeps it in the channel object and never sends it to the firmware).
// With it set, a confirmed transmit is also delivered back as a received message, RxStatus
// TX_MSG_TYPE, carrying the whole frame; raw CAN delivers nothing at all without it.
void loopback_delivers_the_frame() {
    PASSTHRU_MSG out[4]{}; uint32_t count = 0;
    const auto drain = [&](CanReceiver &receiver) {
        count = 0;
        try { receiver.read(out, 4, count, 0); } catch (const Error &error) { CHECK(error.code == ERR_TIMEOUT || error.code == ERR_BUFFER_EMPTY); }
    };
    CanReceiver iso(ISO15765);
    CHECK(!iso.loopback());
    iso.set_loopback(true); CHECK(iso.loopback());
    iso.note_transmit(request_to("000007df0902", ISO15765_FRAME_PAD), 4);
    iso.receive(tx_done(100, 4));
    drain(iso);
    CHECK(count == 2);
    CHECK(out[0].RxStatus == 9 && out[0].DataSize == 4);                       // the TX-done message first
    CHECK(out[1].RxStatus == TX_MSG_TYPE && out[1].TxFlags == ISO15765_FRAME_PAD && out[1].Timestamp == 100);
    CHECK(out[1].DataSize == 6 && hex(Bytes(out[1].Data, out[1].Data + 6)) == "000007df0902" && out[1].ExtraDataIndex == 6);

    CanReceiver raw(CAN);
    PASSTHRU_MSG frame{}; frame.ProtocolID = CAN; frame.DataSize = 12;
    const auto bytes = unhex("000007df0201005555555555"); std::copy(bytes.begin(), bytes.end(), frame.Data);
    raw.note_transmit(frame, 7);
    raw.receive(tx_done(200, 7, channel_node(CAN)));                           // loopback off: nothing
    drain(raw); CHECK(count == 0);
    raw.set_loopback(true);
    raw.note_transmit(frame, 8);
    raw.receive(tx_done(300, 8, channel_node(CAN)));
    drain(raw);
    CHECK(count == 1 && out[0].ProtocolID == CAN && out[0].RxStatus == TX_MSG_TYPE && out[0].Timestamp == 300);
    CHECK(out[0].DataSize == 12 && out[0].ExtraDataIndex == 12 && hex(Bytes(out[0].Data, out[0].Data + 12)) == "000007df0201005555555555");
    // A 29-bit frame reports CAN_29BIT_ID in RxStatus, and its TxFlags.
    PASSTHRU_MSG wide = frame; wide.TxFlags = CAN_29BIT_ID;
    raw.note_transmit(wide, 9);
    raw.receive(tx_done(400, 9, channel_node(CAN)));
    drain(raw); CHECK(count == 1 && out[0].RxStatus == (TX_MSG_TYPE | 0x100) && out[0].TxFlags == CAN_29BIT_ID);
    // The flag can change between the write and the confirmation.
    raw.note_transmit(frame, 10);
    raw.set_loopback(false);
    raw.receive(tx_done(500, 10, channel_node(CAN)));
    drain(raw); CHECK(count == 0);
}
int main() {
    try {
        captured_vin();
        single_frame();
        sequence_gap_kills_receive();
        first_frame_restarts_assembly();
        single_frame_does_not_disturb_assembly();
        foreign_can_id_does_not_disturb_assembly();
        rejects_malformed_first_frames();
        trailing_bytes_are_trimmed();
        vendor_flow_control_filter();
        vendor_transmit();
        receiver_reassembles_captured_vin();
        receiver_reports_transmit_done();
        negative_responses_are_ordinary_messages();
        interleaved_ecu_replies_complete();
        conversation_limit_drops_the_oldest();
        transmit_pairing_by_sequence();
        loopback_delivers_the_frame();
        std::cout << "ISO15765 reassembly against the captured VIN exchange passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
