// Host-side ISO15765 reassembly, tested against the captured VIN exchange. No hardware
// is involved: the adapter generates flow control itself, so reassembly is a pure
// function of the inbound frame sequence.
#include "can.hpp"
#include "isotp.hpp"
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
    CHECK(!"expected an Error");
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
    // A segmented transmit has never been observed, so it is not attempted.
    refused([&] { isotp_transmit(iso_message(0, "000007df0102030405060708"), 1000); }, ERR_NOT_SUPPORTED);
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
        std::cout << "ISO15765 reassembly against the captured VIN exchange passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
