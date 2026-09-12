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
Bytes request(uint16_t opcode, uint16_t sequence, std::span<const uint8_t> payload = {});
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
