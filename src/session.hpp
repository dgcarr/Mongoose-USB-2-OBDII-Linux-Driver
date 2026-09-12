#pragma once
#include "transport.hpp"
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
                  std::chrono::milliseconds timeout = std::chrono::seconds(10));
    void close();
    static uint32_t status(std::span<const uint8_t> response);
private:
    void receive(std::span<const uint8_t> bytes);
    void failed(const std::string &reason);
    std::unique_ptr<Transport> transport_;
    Decoder decoder_;
    std::timed_mutex transaction_;
    std::mutex mutex_, close_mutex_;
    std::condition_variable ready_;
    std::optional<Bytes> response_;
    std::string failure_;
    uint16_t sequence_ = 0;
    uint16_t pending_ = 0;
    size_t minimum_ = 20;
    bool closing_ = false, stopped_ = false;
    std::array<std::chrono::steady_clock::time_point, 256> used_{};
};
}
