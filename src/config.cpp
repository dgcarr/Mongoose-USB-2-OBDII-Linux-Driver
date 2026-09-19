#include "config.hpp"
#include <algorithm>
#include <array>
namespace mongoose {
namespace {
using Where = ConfigRoute::Where;
struct Entry { uint32_t parameter; ConfigRoute route; };
constexpr uint32_t any = 0xffffffffU;
constexpr ConfigRoute fw(uint32_t selector, uint32_t low, uint32_t high, bool ffff = false) {
    return {Where::Firmware, selector, true, low, high, ffff};
}
constexpr ConfigRoute read_only(uint32_t selector) { return {Where::Firmware, selector, false, 0, any, false}; }
constexpr ConfigRoute host_loopback() { return {Where::HostLoopback, 0, true, 0, 1, false}; }
// Raw CAN: vendor base class 10018960 (get) and 10018b30 (set).
constexpr std::array can_table{
    Entry{cfg_data_rate, fw(0x04, 1, 1000000)},            // and only the rates of can_baud_supported()
    Entry{cfg_loopback, host_loopback()},
    Entry{cfg_bit_sample_point, fw(0x14, 68, 80)},         // vendor range {0x44, 0x50}
    Entry{cfg_sync_jump_width, fw(0x15, 0, 100)},
    Entry{cfg_dt_pullup_value, read_only(0x31)},
};
// ISO15765: vendor class 1001ecb0 (get) and 1001ef70 (set). Firmware selectors 0x1b..0x28 are the
// ISO-TP parameters; the values seen on the live sweep (0, 0, 0xffff, 0xffff, 0, 1000...) are J2534 defaults.
constexpr std::array iso_table{
    Entry{cfg_data_rate, fw(0x04, 1, 1000000)},
    Entry{cfg_loopback, host_loopback()},
    Entry{cfg_bit_sample_point, fw(0x14, 80, 80)},         // "Only 80 is valid on ISO15765"
    Entry{cfg_sync_jump_width, fw(0x15, 0, 100)},
    Entry{cfg_iso15765_bs, fw(0x1b, 0, 0xff)},
    Entry{cfg_iso15765_stmin, fw(0x1c, 0, 0xff)},
    Entry{cfg_bs_tx, fw(0x1d, 0, 0xff, true)},
    Entry{cfg_stmin_tx, fw(0x1e, 0, 0xff, true)},
    Entry{cfg_iso15765_wft_max, fw(0x21, 0, 0xff)},
    Entry{cfg_n_br_min, fw(0x26, 0, 0xffff)},
    Entry{cfg_iso15765_pad_value, fw(0x22, 0, 0xff)},
    Entry{cfg_dt_iso15765_pad_byte, fw(0x22, 0, 0xff)},    // the vendor routes both IDs to the same selector
    Entry{cfg_n_as_max, fw(0x23, 1, 0xffff)},
    Entry{cfg_n_ar_max, fw(0x24, 1, 0xffff)},
    Entry{cfg_n_bs_max, fw(0x25, 1, 0xffff)},
    Entry{cfg_n_cr_max, fw(0x28, 1, 0xffff)},
    Entry{cfg_n_cs_min, fw(0x27, 0, 0xffff)},
    Entry{cfg_dt_pullup_value, read_only(0x31)},
    Entry{cfg_dt_half_duplex, read_only(0x2c)},
};
constexpr std::array supported_rates{
    33300u, 33333u, 50000u, 62500u, 83300u, 83333u, 95200u, 95238u, 100000u, 125000u, 166666u, 166667u, 200000u,
    250000u, 500000u, 666666u, 666667u, 1000000u,
};
template<class Table> const ConfigRoute *find(const Table &table, uint32_t parameter) {
    const auto found = std::find_if(table.begin(), table.end(), [&](const Entry &e) { return e.parameter == parameter; });
    return found == table.end() ? nullptr : &found->route;
}
}
ConfigRoute config_route(uint16_t protocol, uint32_t parameter) {
    if (protocol != CAN && protocol != ISO15765) throw Error(ERR_NOT_SUPPORTED, "configuration is implemented for CAN and ISO15765 only");
    // The vendor accepts J1962_PINS only on the *_PS protocol IDs, which this driver does not open.
    if (parameter == cfg_j1962_pins) throw Error(ERR_FAILED, "J1962_PINS only valid for *_PS ProtocolIDs");
    const ConfigRoute *route = protocol == CAN ? find(can_table, parameter) : find(iso_table, parameter);
    if (!route) throw Error(ERR_NOT_SUPPORTED, "configuration parameter not supported on this channel");
    return *route;
}
bool can_baud_supported(uint32_t rate) {
    return std::find(supported_rates.begin(), supported_rates.end(), rate) != supported_rates.end();
}
void config_validate_set(uint16_t protocol, uint32_t parameter, uint32_t value) {
    const auto route = config_route(protocol, parameter);
    if (!route.settable)
        throw Error(ERR_NOT_SUPPORTED, "this driver reads but does not change that parameter (electrical setting)");
    if (parameter == cfg_data_rate && !can_baud_supported(value))
        throw Error(ERR_INVALID_IOCTL_VALUE, "unsupported CAN bit rate");
    if (route.allow_ffff && value == 0xffff) return;
    if (value < route.minimum || value > route.maximum)
        throw Error(ERR_INVALID_IOCTL_VALUE, "configuration value out of range");
}
}
