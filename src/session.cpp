#include "session.hpp"
namespace mongoose {
namespace {
bool response_opcode(uint16_t op) {
    // The vendor dispatcher admits these types before routing/sequence matching.
    return op == 1 || (op >= 0x8003 && op <= 0x8008 && op != 0x8004) ||
           (op >= 0x800b && op <= 0x800e) || (op >= 0x8010 && op <= 0x8015) ||
           op == 0x8100 || op == 0x8102 || op == 0x8103 ||
           (op >= 0x8109 && op <= 0x810c) || op == 0x8111 || op == 0x8112;
}
}
Session::Session(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {
    if (!transport_) throw std::invalid_argument("null transport");
    try {
        transport_->start([this](auto bytes) { receive(bytes); },
                          [this](auto reason) { failed(reason); });
    } catch (...) { try { transport_->stop(); } catch (...) {} throw; }
}
Session::~Session() { try { close(); } catch (...) {} }
void Session::failed(const std::string &reason) {
    std::lock_guard lock(mutex_);
    if (failure_.empty()) failure_ = reason;
    if (can_receiver_) can_receiver_->stop(ERR_DEVICE_NOT_CONNECTED, failure_);
    ready_.notify_all();
}
void Session::receive(std::span<const uint8_t> bytes) {
    std::lock_guard lock(mutex_);
    for (auto &body : decoder_.feed(bytes)) {
        if (can_receiver_) can_receiver_->receive(body);
        // Data and indications cannot satisfy commands.
        if (pending_ && !response_ && body.size() >= minimum_ &&
            response_opcode(le16(body, 4)) && le16(body, 0) == 0 &&
            le16(body, 2) == pending_source_ && le16(body, 6) == pending_) {
            response_ = std::move(body); ready_.notify_all();
        }
    }
}
Bytes Session::command(uint16_t opcode, std::span<const uint8_t> payload,
                       std::chrono::milliseconds timeout, uint16_t destination, uint16_t chan) {
    if (timeout.count() <= 0 || timeout > std::chrono::seconds(60))
        throw std::invalid_argument("command timeout must be 1..60000 ms");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock transaction(transaction_, std::defer_lock);
    if (!transaction.try_lock_until(deadline)) throw Error(ERR_TIMEOUT, "waiting for command slot timed out");
    std::unique_lock state(mutex_);
    if (closing_) throw Error(ERR_DEVICE_NOT_CONNECTED, "session is closing");
    if (!failure_.empty()) throw Error(ERR_DEVICE_NOT_CONNECTED, failure_);
    const auto now = std::chrono::steady_clock::now();
    bool available = false;
    for (unsigned i = 0; i < 255; ++i) {
        sequence_ = static_cast<uint16_t>(sequence_ % 255 + 1);
        if (used_[sequence_] == std::chrono::steady_clock::time_point{} ||
            now - used_[sequence_] >= std::chrono::seconds(10)) { available = true; break; }
    }
    if (!available) throw Error(ERR_EXCEEDED_LIMIT, "all sequence numbers are in the 10-second reuse quarantine");
    const auto wire = encode(request(opcode, sequence_, payload, destination, chan));
    pending_source_ = destination;
    pending_ = sequence_; minimum_ = opcode == 0x100 ? 12 : 20; response_.reset();
    used_[sequence_] = now;
    state.unlock();
    // Compare at full clock resolution: a caller that still has time left must not be
    // failed just because the remainder truncates to zero whole milliseconds.
    const auto left = deadline - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) {
        // Nothing has left the host on this path, so only the sequence slot is lost;
        // the session stays usable.
        state.lock(); pending_ = 0;
        throw Error(ERR_TIMEOUT, "command deadline expired before write");
    }
    // Zero is out of contract for every transport -- libusb reads it as "no timeout",
    // poll(2) as "expire immediately" -- so a sub-millisecond remainder rounds up rather
    // than down. The write may therefore overrun the deadline by under 1 ms; the
    // response wait below still honours the exact deadline.
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(left);
    try {
        transport_->send(wire, static_cast<unsigned>(remaining.count()));
    } catch (...) {
        // A write may have been partially delivered, so the session must be reopened.
        // Keep any root cause the transport already reported; it is more specific.
        state.lock(); pending_ = 0;
        if (failure_.empty()) failure_ = "write failed; reopen before retrying";
        if (can_receiver_) can_receiver_->stop(ERR_DEVICE_NOT_CONNECTED, failure_);
        throw;
    }
    state.lock();
    if (!ready_.wait_until(state, deadline, [this] { return response_ || closing_ || !failure_.empty(); })) {
        pending_ = 0;
        failure_ = "response timed out; reopen to avoid accepting a late response";
        if (can_receiver_) can_receiver_->stop(ERR_DEVICE_NOT_CONNECTED, failure_);
        throw Error(ERR_TIMEOUT, failure_);
    }
    pending_ = 0;
    if (closing_) throw Error(ERR_DEVICE_NOT_CONNECTED, "session closed while waiting");
    if (!failure_.empty()) throw Error(ERR_DEVICE_NOT_CONNECTED, failure_);
    return std::move(*response_);
}
uint32_t Session::status(std::span<const uint8_t> body) {
    if (body.size() < 20) throw Error(ERR_FAILED, "general response shorter than 20 bytes");
    return le32(body, 12);
}
bool Session::usable() {
    std::lock_guard lock(mutex_);
    return !closing_ && failure_.empty();
}
void Session::set_can_receiver(std::shared_ptr<CanReceiver> receiver) {
    std::lock_guard lock(mutex_);
    if (can_receiver_) can_receiver_->stop(ERR_INVALID_CHANNEL_ID, "CAN channel retired");
    can_receiver_ = std::move(receiver);
    if (can_receiver_ && (closing_ || !failure_.empty()))
        can_receiver_->stop(ERR_DEVICE_NOT_CONNECTED, "device connection lost");
}
void Session::close() {
    std::lock_guard close_lock(close_mutex_);
    {
        std::lock_guard state(mutex_);
        if (stopped_) return;
        closing_ = true; ready_.notify_all();
        if (can_receiver_) can_receiver_->stop(ERR_DEVICE_NOT_CONNECTED, "device closed");
    }
    std::lock_guard transaction(transaction_);
    { std::lock_guard state(mutex_); stopped_ = true; }
    transport_->stop();
}
}
