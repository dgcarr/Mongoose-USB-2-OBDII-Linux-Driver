#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <string>
namespace mongoose {
using Bytes = std::vector<uint8_t>;
constexpr size_t max_body = 0x1800;
uint16_t le16(std::span<const uint8_t> data, size_t offset);
uint32_t le32(std::span<const uint8_t> data, size_t offset);
void put16(Bytes &data, size_t offset, uint16_t value);
Bytes encode(std::span<const uint8_t> body);
// Node addressing. The PC is 0 and the board is 1, but channel commands are not
// addressed to the board: the vendor sends (protocol << 8) | board, confirmed on
// Windows by changing only the protocol between two captures - CAN gives 0x0501
// and ISO15765 gives 0x0601 (docs/WINDOWS-FINDINGS.md, section C).
constexpr uint16_t board_node = 1;
constexpr uint16_t channel_node(uint16_t protocol, uint16_t board = board_node) {
    return static_cast<uint16_t>((protocol << 8) | board);
}
// chan (body+8) is 0 for channel management and 1 for data commands; the vendor census
// in docs/WINDOWS-FINDINGS.md section B7 shows no other value. Its meaning is still
// unresolved, so the data path matches the vendor rather than assuming it is ignored.
constexpr uint16_t data_chan = 1;
Bytes request(uint16_t opcode, uint16_t sequence, std::span<const uint8_t> payload = {},
              uint16_t destination = board_node, uint16_t chan = 0);
std::string hex(std::span<const uint8_t> data);
Bytes unhex(const std::string &text);
class Decoder {
public:
    std::vector<Bytes> feed(std::span<const uint8_t> bytes);
    size_t discarded() const { return discarded_; }
    size_t buffered() const { return buffer_.size(); }
private:
    Bytes buffer_;
    size_t discarded_ = 0;
};
}
