#include "codec.hpp"
#include <algorithm>
#include <stdexcept>
#include <cctype>
namespace mongoose {
uint16_t le16(std::span<const uint8_t> d, size_t o) {
    if (o > d.size() || d.size() - o < 2) throw std::out_of_range("short u16 field");
    return static_cast<uint16_t>(d[o] | (static_cast<uint16_t>(d[o+1]) << 8));
}
uint32_t le32(std::span<const uint8_t> d, size_t o) {
    return le16(d, o) | (static_cast<uint32_t>(le16(d, o+2)) << 16);
}
void put16(Bytes &d, size_t o, uint16_t v) {
    if (o > d.size() || d.size() - o < 2) throw std::out_of_range("short u16 destination");
    d[o] = static_cast<uint8_t>(v); d[o+1] = static_cast<uint8_t>(v >> 8);
}
Bytes encode(std::span<const uint8_t> body) {
    if (body.empty() || body.size() > max_body) throw std::invalid_argument("body length outside 1..6144");
    Bytes wire(body.size()+4);
    const auto length = static_cast<uint16_t>(body.size());
    put16(wire, 0, length); put16(wire, 2, length ^ 0x51e6);
    std::copy(body.begin(), body.end(), wire.begin()+4);
    return wire;
}
Bytes request(uint16_t op, uint16_t seq, std::span<const uint8_t> payload, uint16_t destination, uint16_t chan) {
    if (payload.size() > max_body-12 || seq == 0 || seq > 255)
        throw std::invalid_argument("invalid request size or sequence");
    if (destination == 0) throw std::invalid_argument("destination 0 is the PC, not a target");
    // +10 is deliberately initialized to zero rather than copied from vendor stack
    // garbage. That choice is now validated: Windows captures show the vendor
    // sending unrelated values there (0x7719, 0x008f, 0xffff, and 0 - two different
    // values for the same opcode in one run), and the firmware echoes the field
    // back verbatim in the response without acting on it. Zero is accepted exactly
    // as any other value is. See docs/WINDOWS-FINDINGS.md, section B1.
    Bytes body(12+payload.size(), 0);
    put16(body, 0, destination); put16(body, 4, op); put16(body, 6, seq); put16(body, 8, chan);
    std::copy(payload.begin(), payload.end(), body.begin()+12);
    return body;
}
std::vector<Bytes> Decoder::feed(std::span<const uint8_t> bytes) {
    std::vector<Bytes> frames;
    // Incremental ingestion bounds retained input even for arbitrarily large chunks.
    for (uint8_t b : bytes) {
        buffer_.push_back(b);
        while (buffer_.size() >= 4) {
            const auto n = le16(buffer_, 0);
            if (n == 0 || n > max_body || le16(buffer_, 2) != (n ^ 0x51e6)) {
                buffer_.erase(buffer_.begin()); ++discarded_; continue;
            }
            if (buffer_.size() < static_cast<size_t>(n)+4) break;
            frames.emplace_back(buffer_.begin()+4, buffer_.begin()+4+n);
            buffer_.erase(buffer_.begin(), buffer_.begin()+4+n);
        }
    }
    return frames;
}
std::string hex(std::span<const uint8_t> d) {
    constexpr char digits[] = "0123456789abcdef";
    std::string s; s.reserve(d.size()*2);
    for (auto b : d) { s += digits[b >> 4]; s += digits[b & 15]; }
    return s;
}
Bytes unhex(const std::string &s) {
    Bytes result; int high = -1;
    for (const char raw : s) {
        // std::isspace has undefined behaviour for negative char, so widen through
        // unsigned char rather than letting the conversion happen implicitly.
        const auto c = static_cast<unsigned char>(raw);
        if (std::isspace(c)) continue;
        int n = c >= '0' && c <= '9' ? c-'0' :
                c >= 'a' && c <= 'f' ? c-'a'+10 :
                c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
        if (n < 0) throw std::invalid_argument("invalid hex digit");
        if (high < 0) high = n;
        else { result.push_back(static_cast<uint8_t>((high << 4) | n)); high = -1; }
    }
    if (high >= 0) throw std::invalid_argument("odd hex length");
    return result;
}
}
