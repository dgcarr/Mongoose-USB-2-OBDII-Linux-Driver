#pragma once
#include "transport.hpp"
#include "can.hpp"
#include <array>
#include <condition_variable>
#include <mutex>
#include <optional>
namespace mongoose {
class Session {
public:
    explicit Session(std::unique_ptr<Transport> transport);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Bytes command(uint16_t opcode, std::span<const uint8_t> payload = {},
                  std::chrono::milliseconds timeout = std::chrono::seconds(10),
                  uint16_t destination = board_node, uint16_t chan = 0);
    void close();
    bool usable();
    void set_can_receiver(std::shared_ptr<CanReceiver> receiver);
    static uint32_t status(std::span<const uint8_t> response);
private:
    void receive(std::span<const uint8_t> bytes);
    void failed(const std::string &reason);
    std::unique_ptr<Transport> transport_;
    Decoder decoder_;
    std::shared_ptr<CanReceiver> can_receiver_;
    std::timed_mutex transaction_;
    std::mutex mutex_, close_mutex_;
    std::condition_variable ready_;
    std::optional<Bytes> response_;
    std::string failure_;
    uint16_t sequence_ = 0;
    uint16_t pending_ = 0;
    uint16_t pending_source_ = board_node;
    size_t minimum_ = 20;
    bool closing_ = false, stopped_ = false;
    std::array<std::chrono::steady_clock::time_point, 256> used_{};
};
}
