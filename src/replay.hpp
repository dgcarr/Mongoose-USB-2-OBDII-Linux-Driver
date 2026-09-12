#pragma once
#include "transport.hpp"
#include <deque>
#include <istream>
namespace mongoose {
struct Exchange { Bytes out; std::vector<Bytes> in; };
// Explicitly injected offline transport, never an environment-controlled fallback
// for hardware PassThruOpen. Each IN preserves recorded USB chunk boundaries.
class Replay final : public Transport {
public:
    explicit Replay(std::deque<Exchange> exchanges);
    static std::unique_ptr<Replay> read(std::istream &file);
    void start(Receiver receiver, Failure failure) override;
    void send(std::span<const uint8_t> bytes, unsigned timeout_ms) override;
    void stop() override;
    void assert_finished() const;
private:
    std::deque<Exchange> exchanges_;
    Receiver receiver_;
    bool started_ = false, stopped_ = false;
};
}
