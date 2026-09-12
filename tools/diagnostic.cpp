#include "session.hpp"
#include "replay.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <mutex>
namespace {
void show(const char *label, const mongoose::Bytes &body, bool status = true) {
    std::cout << label << " body=" << mongoose::hex(body);
    if (status) std::cout << " status=" << mongoose::Session::status(body);
    std::cout << '\n';
}
void require_status(const mongoose::Bytes &body, bool start = false) {
    auto status = mongoose::Session::status(body);
    if (status != 0 && !(start && status == 7))
        throw mongoose::Error(ERR_FAILED, "adapter returned status " + std::to_string(status));
}
}
int main(int argc, char **argv) {
    try {
        std::string mode, serial, replay_path, trace_path;
        unsigned timeout = 10000;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--list" || arg == "--discover" || arg == "--open-close" || arg == "--inspect") {
                if (!mode.empty()) throw std::invalid_argument("choose exactly one mode");
                mode = arg;
            } else if ((arg == "--serial" || arg == "--replay" || arg == "--trace" || arg == "--timeout-ms") && i+1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--serial") serial = value;
                if (arg == "--replay") replay_path = value;
                if (arg == "--trace") trace_path = value;
                if (arg == "--timeout-ms") {
                    size_t consumed = 0; const auto parsed = std::stoul(value, &consumed);
                    if (consumed != value.size() || parsed == 0 || parsed > 60000) throw std::invalid_argument("invalid timeout");
                    timeout = static_cast<unsigned>(parsed);
                }
            } else {
                std::cerr << "Usage: mongoose-diag --list|--discover|--open-close|--inspect [--serial SERIAL] [--trace FILE] [--timeout-ms 10000] [--replay FILE]\n";
                return arg == "--help" ? 0 : 2;
            }
        }
        if (mode.empty()) throw std::invalid_argument("an explicit mode is required");
        if (mode == "--list") {
            if (!replay_path.empty() || !trace_path.empty() || !serial.empty()) throw std::invalid_argument("--list takes no device or file options");
            for (const auto &device : mongoose::usb_devices())
                std::cout << device.location << " serial=" << device.serial << " access=" << (device.error.empty() ? "ok" : device.error) << '\n';
            return 0;
        }
        if (!replay_path.empty() && (!serial.empty() || !trace_path.empty()))
            throw std::invalid_argument("replay cannot be combined with hardware options");
        std::ofstream trace;
        if (!trace_path.empty()) {
            trace.open(trace_path, std::ios::out | std::ios::app);
            if (!trace) throw std::runtime_error("cannot open trace file");
            trace << "# session: steady-clock microseconds direction hex; not a USBPcap capture\n";
        }
        std::mutex trace_mutex;
        const auto begin = std::chrono::steady_clock::now();
        auto logger = [&](const std::string &direction, std::span<const uint8_t> bytes) {
            std::lock_guard lock(trace_mutex);
            if (trace.is_open()) {
                const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
                trace << us << ' ' << direction << ' ' << mongoose::hex(bytes) << '\n'; trace.flush();
                if (!trace) throw std::runtime_error("trace write failed");
            }
        };
        std::unique_ptr<mongoose::Transport> transport;
        mongoose::Replay *replay = nullptr;
        if (!replay_path.empty()) {
            std::ifstream input(replay_path);
            if (!input) throw std::runtime_error("cannot open replay");
            auto source = mongoose::Replay::read(input); replay = source.get(); transport = std::move(source);
        } else transport = mongoose::usb_transport(serial, logger);
        mongoose::Session session(std::move(transport));
        std::exception_ptr failure;
        bool opened = false;
        try {
            const auto deadline = std::chrono::milliseconds(timeout);
            if (mode == "--discover") {
                auto echo = session.command(0x100, {}, deadline); show("echo", echo, false);
                auto info = session.command(0x109, {}, deadline); show("board-info", info); require_status(info);
            } else {
                auto start = session.command(0x103, {}, deadline); show("start", start); require_status(start, true);
                auto open = session.command(3, {}, deadline); show("open", open); require_status(open); opened = true;
                auto info = session.command(0x109, {}, deadline); show("board-info", info); require_status(info);
                if (mode == "--inspect") {
                    for (uint8_t selector : std::array<uint8_t, 4>{0x2b, 0x2a, 3, 2}) {
                        mongoose::Bytes payload{selector, 0, 0, 0};
                        auto value = session.command(0xc, payload, deadline);
                        std::cout << "selector=" << static_cast<unsigned>(selector) << ' ';
                        show("value", value); require_status(value);
                    }
                }
            }
        } catch (...) { failure = std::current_exception(); }
        if (opened) {
            try {
                auto close = session.command(5, {}, std::chrono::milliseconds(timeout));
                show("close", close); require_status(close);
            } catch (const std::exception &e) {
                std::cerr << "close command: " << e.what() << '\n';
                if (!failure) failure = std::current_exception();
            }
        }
        try { session.close(); } catch (const std::exception &e) {
            std::cerr << "USB cleanup: " << e.what() << '\n';
            if (!failure) failure = std::current_exception();
        }
        if (failure) std::rethrow_exception(failure);
        if (replay) replay->assert_finished();
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
