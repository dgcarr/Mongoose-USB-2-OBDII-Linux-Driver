#include "codec.hpp"
#include "can.hpp"
#include <algorithm>
#include <cstdlib>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mongoose::Decoder decoder;
    mongoose::CanReceiver receiver;
    for (size_t offset = 0; offset < size;) {
        const size_t length = std::min(size-offset, static_cast<size_t>(1+data[offset]));
        auto frames = decoder.feed(std::span(data+offset, length));
        if (decoder.buffered() > mongoose::max_body+3) std::abort();
        for (const auto &body : frames) {
            receiver.receive(body);
            mongoose::Decoder check;
            auto roundtrip = check.feed(mongoose::encode(body));
            if (roundtrip.size() != 1 || roundtrip[0] != body) std::abort();
        }
        offset += length;
    }
    PASSTHRU_MSG message{};
    uint32_t count = 0;
    try { receiver.read(&message, 1, count, 0); } catch (const mongoose::Error &) {}
    if (count && (message.DataSize < 4 || message.DataSize > 12 || message.ExtraDataIndex != message.DataSize)) std::abort();
    return 0;
}
