#include "codec.hpp"
#include <algorithm>
#include <cstdlib>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mongoose::Decoder decoder;
    for (size_t offset = 0; offset < size;) {
        const size_t length = std::min(size-offset, static_cast<size_t>(1+data[offset]));
        auto frames = decoder.feed(std::span(data+offset, length));
        if (decoder.buffered() > mongoose::max_body+3) std::abort();
        for (const auto &body : frames) {
            mongoose::Decoder check;
            auto roundtrip = check.feed(mongoose::encode(body));
            if (roundtrip.size() != 1 || roundtrip[0] != body) std::abort();
        }
        offset += length;
    }
    return 0;
}
