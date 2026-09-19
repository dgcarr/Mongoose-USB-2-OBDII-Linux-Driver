// The SConfig -> firmware selector map, pinned against the vendor's own getters and setters
// (analysis/decompiled/config: 1001ecb0/1001ef70 for ISO15765, 10018960/10018b30 for CAN) and against
// the values a live firmware sweep found behind those selectors (PROTOCOL.md section 7d).
#include "config.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace mongoose;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " at " + std::to_string(__LINE__)); } while (0)
namespace {
int32_t route_error(uint16_t protocol, uint32_t parameter) {
    try { config_route(protocol, parameter); } catch (const Error &error) { return error.code; }
    return 0;
}
int32_t set_error(uint16_t protocol, uint32_t parameter, uint32_t value) {
    try { config_validate_set(protocol, parameter, value); } catch (const Error &error) { return error.code; }
    return 0;
}
struct Expected { uint32_t parameter, selector; };
void iso_selectors() {
    // Every ISO15765 case of vendor 1001ecb0, in the vendor's order.
    const std::vector<Expected> map{
        {cfg_data_rate, 0x04}, {cfg_bit_sample_point, 0x14}, {cfg_sync_jump_width, 0x15},
        {cfg_iso15765_bs, 0x1b}, {cfg_iso15765_stmin, 0x1c}, {cfg_bs_tx, 0x1d}, {cfg_stmin_tx, 0x1e},
        {cfg_iso15765_wft_max, 0x21}, {cfg_n_br_min, 0x26}, {cfg_iso15765_pad_value, 0x22},
        {cfg_n_as_max, 0x23}, {cfg_n_ar_max, 0x24}, {cfg_n_bs_max, 0x25}, {cfg_n_cr_max, 0x28}, {cfg_n_cs_min, 0x27},
        {cfg_dt_pullup_value, 0x31}, {cfg_dt_iso15765_pad_byte, 0x22}, {cfg_dt_half_duplex, 0x2c},
    };
    for (const auto &[parameter, selector] : map) {
        const auto route = config_route(ISO15765, parameter);
        CHECK(route.where == ConfigRoute::Where::Firmware && route.selector == selector);
    }
    CHECK(config_route(ISO15765, cfg_loopback).where == ConfigRoute::Where::HostLoopback);
    // The sweep found firmware selectors 0x1b..0x28 on an ISO15765 channel and nowhere else; the ones the
    // map names are exactly those that answered there (0x26 and 0x27 answered 0, 0x2c answered 0).
    for (uint32_t parameter : {cfg_iso15765_bs, cfg_iso15765_stmin, cfg_bs_tx, cfg_stmin_tx, cfg_iso15765_wft_max,
                               cfg_n_br_min, cfg_iso15765_pad_value, cfg_n_as_max, cfg_n_ar_max, cfg_n_bs_max,
                               cfg_n_cr_max, cfg_n_cs_min})
        CHECK(route_error(CAN, parameter) == ERR_NOT_SUPPORTED);   // ISO-TP parameters do not exist on raw CAN
}
void can_selectors() {
    const std::vector<Expected> map{
        {cfg_data_rate, 0x04}, {cfg_bit_sample_point, 0x14}, {cfg_sync_jump_width, 0x15}, {cfg_dt_pullup_value, 0x31},
    };
    for (const auto &[parameter, selector] : map) {
        const auto route = config_route(CAN, parameter);
        CHECK(route.where == ConfigRoute::Where::Firmware && route.selector == selector);
    }
    CHECK(config_route(CAN, cfg_loopback).where == ConfigRoute::Where::HostLoopback);
    // Vendor 10018960: J1962_PINS is an error off the *_PS protocols, anything unlisted is unsupported.
    CHECK(route_error(CAN, cfg_j1962_pins) == ERR_FAILED && route_error(ISO15765, cfg_j1962_pins) == ERR_FAILED);
    for (uint32_t parameter : {0x07u, 0x09u, 0x13u, 0x8010u, 0xc002u, 0x10000005u, 0x10000008u, 0u})
        CHECK(route_error(CAN, parameter) == ERR_NOT_SUPPORTED && route_error(ISO15765, parameter) == ERR_NOT_SUPPORTED);
    CHECK(route_error(J1850VPW, cfg_data_rate) == ERR_NOT_SUPPORTED && route_error(ISO9141, cfg_data_rate) == ERR_NOT_SUPPORTED);
}
void ranges() {
    // ISO15765 setter 1001ef70: one-byte BS, STMIN and WFT_MAX; BS_TX/STMIN_TX also 0xFFFF; N_* timeouts >= 1.
    CHECK(set_error(ISO15765, cfg_iso15765_bs, 0) == 0 && set_error(ISO15765, cfg_iso15765_bs, 255) == 0);
    CHECK(set_error(ISO15765, cfg_iso15765_bs, 256) == ERR_INVALID_IOCTL_VALUE);
    CHECK(set_error(ISO15765, cfg_iso15765_stmin, 255) == 0 && set_error(ISO15765, cfg_iso15765_stmin, 256) == ERR_INVALID_IOCTL_VALUE);
    CHECK(set_error(ISO15765, cfg_bs_tx, 0xffff) == 0 && set_error(ISO15765, cfg_bs_tx, 0xfffe) == ERR_INVALID_IOCTL_VALUE);
    CHECK(set_error(ISO15765, cfg_stmin_tx, 0xffff) == 0 && set_error(ISO15765, cfg_stmin_tx, 100) == 0);
    CHECK(set_error(ISO15765, cfg_iso15765_wft_max, 255) == 0 && set_error(ISO15765, cfg_iso15765_wft_max, 256) == ERR_INVALID_IOCTL_VALUE);
    CHECK(set_error(ISO15765, cfg_n_br_min, 0xffff) == 0 && set_error(ISO15765, cfg_n_br_min, 0x10000) == ERR_INVALID_IOCTL_VALUE);
    CHECK(set_error(ISO15765, cfg_iso15765_pad_value, 0x55) == 0 && set_error(ISO15765, cfg_dt_iso15765_pad_byte, 0x100) == ERR_INVALID_IOCTL_VALUE);
    for (uint32_t parameter : {cfg_n_as_max, cfg_n_ar_max, cfg_n_bs_max, cfg_n_cr_max}) {
        CHECK(set_error(ISO15765, parameter, 0) == ERR_INVALID_IOCTL_VALUE);
        CHECK(set_error(ISO15765, parameter, 1) == 0 && set_error(ISO15765, parameter, 0xffff) == 0);
        CHECK(set_error(ISO15765, parameter, 0x10000) == ERR_INVALID_IOCTL_VALUE);
    }
    CHECK(set_error(ISO15765, cfg_n_cs_min, 0) == 0);
    // Sample point: 80 only on ISO15765; 68..80 on CAN. Sync jump width 0..100 on both.
    CHECK(set_error(ISO15765, cfg_bit_sample_point, 80) == 0 && set_error(ISO15765, cfg_bit_sample_point, 79) == ERR_INVALID_IOCTL_VALUE);
    CHECK(set_error(CAN, cfg_bit_sample_point, 68) == 0 && set_error(CAN, cfg_bit_sample_point, 80) == 0);
    CHECK(set_error(CAN, cfg_bit_sample_point, 67) == ERR_INVALID_IOCTL_VALUE && set_error(CAN, cfg_bit_sample_point, 81) == ERR_INVALID_IOCTL_VALUE);
    for (uint16_t protocol : {static_cast<uint16_t>(CAN), static_cast<uint16_t>(ISO15765)}) {
        CHECK(set_error(protocol, cfg_sync_jump_width, 100) == 0 && set_error(protocol, cfg_sync_jump_width, 101) == ERR_INVALID_IOCTL_VALUE);
        CHECK(set_error(protocol, cfg_loopback, 0) == 0 && set_error(protocol, cfg_loopback, 1) == 0 && set_error(protocol, cfg_loopback, 2) == ERR_INVALID_IOCTL_VALUE);
        CHECK(set_error(protocol, cfg_data_rate, 500000) == 0 && set_error(protocol, cfg_data_rate, 0) == ERR_INVALID_IOCTL_VALUE);
        CHECK(set_error(protocol, cfg_data_rate, 1000001) == ERR_INVALID_IOCTL_VALUE);
        // Readable, not settable: electrical settings this driver will not change.
        CHECK(set_error(protocol, cfg_dt_pullup_value, 0) == ERR_NOT_SUPPORTED);
    }
    CHECK(set_error(ISO15765, cfg_dt_half_duplex, 1) == ERR_NOT_SUPPORTED);
    // NON_VOLATILE_STORE_2..10 (0xC002-0xC00A) write non-volatile memory and are not in either table.
    for (uint32_t parameter = 0xc002; parameter <= 0xc00a; ++parameter)
        CHECK(set_error(CAN, parameter, 1) == ERR_NOT_SUPPORTED && set_error(ISO15765, parameter, 1) == ERR_NOT_SUPPORTED);
}
}
int main() {
    try {
        iso_selectors(); can_selectors(); ranges();
        std::cout << "SConfig map and ranges match the vendor's getters and setters\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
