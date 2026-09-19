#include "session.hpp"
#include "can.hpp"
#include "replay.hpp"
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
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
// A failed command carries NUL-terminated ASCII at body+20; see PROTOCOL.md section 7a.
std::string firmware_text(const mongoose::Bytes &body) {
    if (body.size() <= 20) return {};
    const auto *text = reinterpret_cast<const char *>(body.data()) + 20;
    return std::string(text, strnlen(text, body.size()-20));
}
std::string outcome(const mongoose::Bytes &body) {
    const auto status = mongoose::Session::status(body);
    std::ostringstream line;
    line << "status=0x" << std::hex << status << std::dec;
    // Only a failure puts text at body+20. On success the same offset holds binary
    // payload -- a handle, or an echoed selector -- which must not be printed as text.
    if (status) {
        const auto text = firmware_text(body);
        if (!text.empty()) line << " text=\"" << text << '"';
    }
    return line.str();
}
// Reuses the shipping filter encoder rather than rebuilding the payload, so the probe
// exercises the same bytes the library sends. Mask 0x7ff selects one 11-bit ID.
mongoose::Bytes pass_filter(uint32_t can_id) {
    PASSTHRU_MSG mask{}, pattern{};
    mask.ProtocolID = pattern.ProtocolID = CAN;
    mask.DataSize = pattern.DataSize = 4;
    mask.Data[2] = 0x07; mask.Data[3] = 0xff;
    pattern.Data[2] = static_cast<uint8_t>(can_id >> 8);
    pattern.Data[3] = static_cast<uint8_t>(can_id);
    return mongoose::can_pass_filter(mask, pattern, 0);
}
mongoose::Bytes table_entry(uint32_t selector, uint32_t handle) {
    mongoose::Bytes payload(8, 0);
    mongoose::put16(payload, 0, static_cast<uint16_t>(selector));
    mongoose::put16(payload, 4, static_cast<uint16_t>(handle));
    mongoose::put16(payload, 6, static_cast<uint16_t>(handle >> 16));
    return payload;
}
// Session holds each of the 255 sequence numbers for ten seconds before reuse, so any
// burst longer than that exhausts the space and is refused. That is a deliberate library
// choice (docs/VALIDATION.md); a bench probe that wants the adapter's limit rather than
// ours simply waits the quarantine out.
mongoose::Bytes patient_command(mongoose::Session &session, uint16_t opcode,
                                std::span<const uint8_t> payload,
                                std::chrono::milliseconds deadline, uint16_t node) {
    for (unsigned attempt = 0; ; ++attempt) {
        try {
            return session.command(opcode, payload, deadline, node);
        } catch (const mongoose::Error &error) {
            if (error.code != ERR_EXCEEDED_LIMIT || attempt == 15) throw;
            std::cout << "sequence quarantine full after " << attempt
                      << " waits; pausing 1s" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
// Bench-only. Every command here is a table or read operation on an open CAN channel:
// nothing is transmitted, no value is written, and no destructive opcode is reachable.
class FilterProbe {
public:
    FilterProbe(mongoose::Session &session, std::chrono::milliseconds deadline, unsigned ceiling)
        : session_(session), deadline_(deadline), ceiling_(ceiling) {}
    void run() {
        const auto capacity = fill("capacity");
        std::cout << "capacity: " << capacity << " pass filters accepted\n";
        drain();
        auto reopened = add(0x700);
        std::cout << "after-removal re-add " << outcome(reopened) << '\n';
        if (!mongoose::Session::status(reopened) && reopened.size() >= 24)
            handles_.push_back(mongoose::le32(reopened, 20));
        drain();
        clear_table();
        auto probed = value(0x2f);
        std::cout << "selector 0x2f " << outcome(probed) << " value=0x" << std::hex
                  << last_value_ << std::dec << '\n';
    }
private:
    unsigned fill(const char *label) {
        for (unsigned i = 0; i < ceiling_; ++i) {
            auto response = add(0x100 + i);
            const auto status = mongoose::Session::status(response);
            if (status) {
                std::cout << label << " refused at " << handles_.size() << ' ' << outcome(response) << '\n';
                return static_cast<unsigned>(handles_.size());
            }
            if (response.size() < 24)
                throw mongoose::Error(ERR_FAILED, "filter response carried no handle");
            handles_.push_back(mongoose::le32(response, 20));
            // Per-iteration progress: a stalled fill must be visible while it runs.
            std::cout << label << ' ' << handles_.size() << " handle=0x" << std::hex
                      << handles_.back() << std::dec << std::endl;
        }
        std::cout << label << " reached the probe ceiling of " << ceiling_
                  << " without refusal\n";
        return ceiling_;
    }
    void drain() {
        for (const auto handle : handles_) {
            auto response = patient_command(session_, 0x0e, table_entry(0, handle), deadline_, channel);
            if (mongoose::Session::status(response))
                std::cout << "remove 0x" << std::hex << handle << std::dec << ' ' << outcome(response) << '\n';
        }
        std::cout << "removed " << handles_.size() << " filters\n";
        handles_.clear();
    }
    void clear_table() {
        for (unsigned i = 0; i < 3; ++i) {
            auto response = add(0x200 + i);
            if (mongoose::Session::status(response)) return;
            handles_.push_back(mongoose::le32(response, 20));
        }
        mongoose::Bytes selector(4, 0);
        auto cleared = patient_command(session_, 0x10, selector, deadline_, channel);
        std::cout << "cTableClear selector 0 " << outcome(cleared) << '\n';
        auto stale = patient_command(session_, 0x0e, table_entry(0, handles_.front()), deadline_, channel);
        std::cout << "remove after clear " << outcome(stale) << '\n';
        auto refilled = add(0x300);
        std::cout << "add after clear " << outcome(refilled) << '\n';
        handles_.clear();
        if (!mongoose::Session::status(refilled) && refilled.size() >= 24) {
            handles_.push_back(mongoose::le32(refilled, 20));
            drain();
        }
    }
    mongoose::Bytes add(uint32_t can_id) {
        return patient_command(session_, 0x0d, pass_filter(can_id), deadline_, channel);
    }
    mongoose::Bytes value(uint8_t selector) {
        const mongoose::Bytes payload{selector, 0, 0, 0};
        auto response = patient_command(session_, 0xc, payload, deadline_, mongoose::board_node);
        last_value_ = response.size() >= 28 ? mongoose::le32(response, 24) : 0;
        return response;
    }
    static constexpr uint16_t channel = mongoose::channel_node(CAN);
    mongoose::Session &session_;
    std::chrono::milliseconds deadline_;
    unsigned ceiling_;
    std::vector<uint32_t> handles_;
    uint32_t last_value_ = 0;
};
}
int main(int argc, char **argv) {
    try {
        std::string mode, serial, device_selector, replay_path, trace_path;
        unsigned timeout = 10000, ceiling = 512;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--list" || arg == "--discover" || arg == "--open-close" ||
                arg == "--inspect" || arg == "--filter-probe" || arg == "--transmit-probe" || arg == "--read-burst" ||
                arg == "--value-sweep") {
                if (!mode.empty()) throw std::invalid_argument("choose exactly one mode");
                mode = arg;
            } else if ((arg == "--serial" || arg == "--device" || arg == "--replay" || arg == "--trace" ||
                        arg == "--timeout-ms" || arg == "--filter-ceiling") && i+1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--serial") serial = value;
                if (arg == "--device") device_selector = value;
                if (arg == "--replay") replay_path = value;
                if (arg == "--trace") trace_path = value;
                if (arg == "--timeout-ms") {
                    size_t consumed = 0; const auto parsed = std::stoul(value, &consumed);
                    if (consumed != value.size() || parsed == 0 || parsed > 60000) throw std::invalid_argument("invalid timeout");
                    timeout = static_cast<unsigned>(parsed);
                }
                if (arg == "--filter-ceiling") {
                    size_t consumed = 0; const auto parsed = std::stoul(value, &consumed);
                    if (consumed != value.size() || parsed == 0 || parsed > 100000) throw std::invalid_argument("invalid ceiling");
                    ceiling = static_cast<unsigned>(parsed);
                }
            } else {
                std::cerr << "Usage: mongoose-diag --list|--discover|--open-close|--inspect|--filter-probe|--transmit-probe|--read-burst|--value-sweep "
                             "[--device SELECTOR] [--serial SERIAL] [--trace FILE] [--timeout-ms 10000] [--replay FILE]\n"
                             "  SELECTOR: serial:S | tty:[serial:S|/dev/ttyACMn] | usb:[serial:S]  (default: tty)\n"
                             "  --filter-probe opens a CAN channel and fills the filter table; it never transmits.\n"
                             "  --filter-ceiling N caps that fill (default 512).\n"
                             "  --transmit-probe sends ONE OBD-II mode 01 PID 00 frame on a CAN channel.\n"
                             "  --value-sweep reads firmware cGetValue selectors 0..0x7f on the board and on an open CAN\n"
                             "                channel. Read-only: nothing is written or transmitted.\n";
                return arg == "--help" ? 0 : 2;
            }
        }
        if (mode.empty()) throw std::invalid_argument("an explicit mode is required");
        if (mode == "--list") {
            if (!replay_path.empty() || !trace_path.empty() || !serial.empty() || !device_selector.empty())
                throw std::invalid_argument("--list takes no device or file options");
            for (const auto backend : {mongoose::Backend::Tty, mongoose::Backend::Usb}) {
                const char *label = backend == mongoose::Backend::Tty ? "tty" : "usb";
                try {
                    for (const auto &device : mongoose::list_devices(backend))
                        std::cout << label << ' ' << device.location << " serial=" << device.serial
                                  << " access=" << (device.error.empty() ? "ok" : device.error) << '\n';
                } catch (const mongoose::Error &) {
                    // A build without libusb simply has no usb section to report.
                }
            }
            return 0;
        }
        if (!replay_path.empty() && (!serial.empty() || !trace_path.empty()))
            throw std::invalid_argument("replay cannot be combined with hardware options");
        if (!replay_path.empty() && !device_selector.empty())
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
        } else {
            // --serial is sugar for serial:S; --device takes the full selector grammar.
            if (!serial.empty() && !device_selector.empty())
                throw std::invalid_argument("use either --serial or --device, not both");
            const std::string name = !device_selector.empty() ? device_selector
                                   : serial.empty() ? std::string() : "serial:" + serial;
            transport = mongoose::open_transport(mongoose::parse_selector(name), logger);
        }
        mongoose::Session session(std::move(transport));
        std::exception_ptr failure;
        bool opened = false, channel_opened = false, channel_opened_iso = false;
        // The probe stops here even if the firmware never refuses, so a wrong reply
        // cannot turn the fill into an unbounded loop against the adapter.
        const unsigned filter_ceiling = ceiling;
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
                if (mode == "--read-burst") {
                    // Read-only firmware-version reads, as fast as the link allows. The
                    // point is sequence reuse under sustained load: the 255-value space
                    // recycles every 255 commands, so a stale or duplicated response
                    // would show up here as a mismatched echo.
                    const auto started = std::chrono::steady_clock::now();
                    unsigned mismatched = 0;
                    for (unsigned i = 0; i < ceiling; ++i) {
                        const mongoose::Bytes selector{0x2b, 0, 0, 0};
                        auto response = session.command(0xc, selector, deadline);
                        require_status(response);
                        if (response.size() < 28 || mongoose::le32(response, 20) != 0x2b) ++mismatched;
                        if ((i+1) % 1000 == 0)
                            std::cout << "burst " << i+1 << '/' << ceiling << std::endl;
                    }
                    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
                    std::cout << "burst " << ceiling << " commands in " << seconds << " s = "
                              << static_cast<unsigned>(ceiling/seconds) << " cmd/s, "
                              << mismatched << " mismatched responses\n";
                }
                if (mode == "--transmit-probe") {
                    constexpr auto channel = mongoose::channel_node(CAN);
                    auto channel_open = session.command(6, mongoose::unhex("0000000020a10700"), deadline, channel);
                    show("open-channel", channel_open); require_status(channel_open);
                    channel_opened = true;
                    auto pins = session.command(0x12, mongoose::unhex("01000000060000000e000000"), deadline, channel);
                    show("set-pin", pins); require_status(pins);
                    // OBD-II mode 01 PID 00 to the functional address: the standard
                    // read-only "what do you support" query, harmless on a real bus and
                    // the same request the Windows reference captured. A raw CAN channel
                    // has no ISO-TP layer, so the single frame carries its own PCI byte
                    // (02) and is padded to 8 bytes; unframed, an ECU would ignore it.
                    auto listener = std::make_shared<mongoose::CanReceiver>();
                    session.set_can_receiver(listener);
                    PASSTHRU_MSG frame{};
                    frame.ProtocolID = CAN; frame.DataSize = 12;
                    const auto bytes = mongoose::unhex("000007df0201005555555555");
                    std::copy(bytes.begin(), bytes.end(), frame.Data);
                    // Recorded before it goes out, as the library does, so the adapter's confirmation pairs with it.
                    auto sent = session.command(8, mongoose::can_transmit(frame, 1000), deadline,
                                                channel, mongoose::data_chan,
                                                [&](uint16_t sequence) { listener->note_transmit(frame, sequence); });
                    show("outbound", sent);
                    const auto status = mongoose::Session::status(sent);
                    std::cout << "transmit " << outcome(sent)
                              << (status == 0x100 ? " (queued)" : status ? " (refused)" : " (accepted)") << '\n';
                    // Whether iMsgTxDone ever arrives without a bus to acknowledge the
                    // frame is exactly what this probe exists to find out.
                    for (unsigned elapsed = 0; elapsed < 20; ++elapsed) {
                        if (listener->transmitted()) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    std::cout << "iMsgTxDone indications: " << listener->transmitted() << '\n';
                    PASSTHRU_MSG received{}; uint32_t count = 1;
                    try {
                        listener->read(&received, 1, count, 100);
                        std::cout << "received " << count << " frames, first size " << received.DataSize << '\n';
                    } catch (const mongoose::Error &error) {
                        std::cout << "receive: " << error.what() << " (expected: this probe installs no pass filter)\n";
                    }
                    session.set_can_receiver({});
                }
                if (mode == "--value-sweep") {
                    // cGetValue (0x0c) takes exactly a four-byte selector and answers with the selector at
                    // +20 and the value at +24, or with NUL-terminated ASCII at +20 on refusal. It is the
                    // adapter's own read path, so an unknown selector costs one error string.
                    const auto sweep = [&](const char *label, uint16_t node) {
                        for (unsigned selector = 0; selector < 0x80; ++selector) {
                            const mongoose::Bytes payload{static_cast<uint8_t>(selector), 0, 0, 0};
                            const auto response = session.command(0xc, payload, deadline, node);
                            const auto status = mongoose::Session::status(response);
                            std::cout << label << " selector=0x" << std::hex << selector << std::dec << " status=" << status;
                            if (status == 0 && response.size() >= 28)
                                std::cout << " echo=0x" << std::hex << mongoose::le32(response, 20)
                                          << " value=0x" << mongoose::le32(response, 24) << std::dec
                                          << " (" << mongoose::le32(response, 24) << ")";
                            else if (response.size() > 20) {
                                std::string text;
                                for (size_t i = 20; i < response.size() && response[i]; ++i)
                                    text += response[i] >= 32 && response[i] < 127 ? static_cast<char>(response[i]) : '.';
                                std::cout << " text=\"" << text << '"';
                            }
                            std::cout << '\n';
                        }
                    };
                    sweep("board", mongoose::board_node);
                    constexpr auto channel = mongoose::channel_node(CAN);
                    auto channel_open = session.command(6, mongoose::unhex("0000000020a10700"), deadline, channel);
                    show("open-channel", channel_open); require_status(channel_open);
                    channel_opened = true;
                    auto pins = session.command(0x12, mongoose::unhex("01000000060000000e000000"), deadline, channel);
                    show("set-pin", pins); require_status(pins);
                    sweep("channel", channel);
                    // An ISO15765 channel has its own parameters (block size, STmin, ...), so sweep one
                    // too. Only one CAN-family channel can be open, so close the CAN one first.
                    auto closed = session.command(7, {}, deadline, channel);
                    show("close-channel", closed); require_status(closed);
                    channel_opened = false;
                    constexpr auto iso = mongoose::channel_node(ISO15765);
                    auto iso_open = session.command(6, mongoose::unhex("0000000020a10700"), deadline, iso);
                    show("open-iso15765", iso_open); require_status(iso_open);
                    channel_opened_iso = true;
                    auto iso_pins = session.command(0x12, mongoose::unhex("01000000060000000e000000"), deadline, iso);
                    show("set-pin", iso_pins); require_status(iso_pins);
                    sweep("iso15765", iso);
                }
                if (mode == "--filter-probe") {
                    constexpr auto channel = mongoose::channel_node(CAN);
                    // Same open sequence PassThruConnect uses: flags 0, 500000 baud, then pins.
                    auto channel_open = session.command(6, mongoose::unhex("0000000020a10700"), deadline, channel);
                    show("open-channel", channel_open); require_status(channel_open);
                    channel_opened = true;
                    auto pins = session.command(0x12, mongoose::unhex("01000000060000000e000000"), deadline, channel);
                    show("set-pin", pins); require_status(pins);
                    FilterProbe(session, deadline, filter_ceiling).run();
                }
            }
        } catch (...) { failure = std::current_exception(); }
        if (channel_opened) {
            try {
                auto closed = patient_command(session, 7, {}, std::chrono::milliseconds(timeout), mongoose::channel_node(CAN));
                show("close-channel", closed); require_status(closed);
            } catch (const std::exception &e) {
                std::cerr << "close channel: " << e.what() << '\n';
                if (!failure) failure = std::current_exception();
            }
        }
        if (channel_opened_iso) {
            try {
                auto closed = patient_command(session, 7, {}, std::chrono::milliseconds(timeout), mongoose::channel_node(ISO15765));
                show("close-iso15765", closed); require_status(closed);
            } catch (const std::exception &e) {
                std::cerr << "close ISO15765 channel: " << e.what() << '\n';
                if (!failure) failure = std::current_exception();
            }
        }
        if (opened) {
            try {
                auto close = patient_command(session, 5, {}, std::chrono::milliseconds(timeout), mongoose::board_node);
                show("close", close); require_status(close);
            } catch (const std::exception &e) {
                std::cerr << "close command: " << e.what() << '\n';
                if (!failure) failure = std::current_exception();
            }
        }
        try { session.close(); } catch (const std::exception &e) {
            std::cerr << "transport cleanup: " << e.what() << '\n';
            if (!failure) failure = std::current_exception();
        }
        if (failure) std::rethrow_exception(failure);
        if (replay) replay->assert_finished();
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
