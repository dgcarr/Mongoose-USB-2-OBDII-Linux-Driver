#include "replay.hpp"
#include <algorithm>
#include <sstream>
namespace mongoose {
Replay::Replay(std::deque<Exchange> exchanges) : exchanges_(std::move(exchanges)) {}
std::unique_ptr<Replay> Replay::read(std::istream &file) {
    std::deque<Exchange> exchanges;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line); std::string direction, data;
        row >> direction; std::getline(row, data);
        if (direction == "OUT") exchanges.push_back({unhex(data), {}});
        else if (direction == "IN" && !exchanges.empty()) exchanges.back().in.push_back(unhex(data));
        else throw std::invalid_argument("replay requires OUT hex followed by IN hex chunks");
    }
    if (file.bad()) throw std::runtime_error("failed to read replay");
    return std::make_unique<Replay>(std::move(exchanges));
}
void Replay::start(Receiver receiver, Failure) {
    if (started_ || stopped_) throw std::logic_error("replay cannot restart");
    receiver_ = std::move(receiver); started_ = true;
}
void Replay::send(std::span<const uint8_t> bytes, unsigned) {
    if (!started_ || stopped_) throw Error(ERR_DEVICE_NOT_CONNECTED, "replay stopped");
    if (exchanges_.empty() || !std::equal(bytes.begin(), bytes.end(), exchanges_.front().out.begin(), exchanges_.front().out.end()))
        throw Error(ERR_FAILED, "replay OUT differs from expected frame");
    auto exchange = std::move(exchanges_.front()); exchanges_.pop_front();
    for (const auto &chunk : exchange.in) receiver_(chunk);
}
void Replay::stop() { stopped_ = true; }
void Replay::assert_finished() const {
    if (!exchanges_.empty()) throw Error(ERR_FAILED, "replay has unconsumed exchanges");
}
}
