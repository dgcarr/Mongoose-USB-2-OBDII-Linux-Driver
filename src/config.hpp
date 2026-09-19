#pragma once
#include "transport.hpp"
#include "mongoose/j2534.h"
namespace mongoose {
// SConfig parameter IDs (vendor table FUN_10038b70, extracted in analysis/ENUMS.md).
enum : uint32_t {
    cfg_data_rate = 0x01, cfg_loopback = 0x03, cfg_bit_sample_point = 0x17, cfg_sync_jump_width = 0x18,
    cfg_iso15765_bs = 0x1e, cfg_iso15765_stmin = 0x1f, cfg_bs_tx = 0x22, cfg_stmin_tx = 0x23,
    cfg_iso15765_wft_max = 0x25, cfg_n_br_min = 0x2a, cfg_iso15765_pad_value = 0x2b,
    cfg_n_as_max = 0x2c, cfg_n_ar_max = 0x2d, cfg_n_bs_max = 0x2e, cfg_n_cr_max = 0x2f, cfg_n_cs_min = 0x30,
    cfg_j1962_pins = 0x8001,
    cfg_dt_pullup_value = 0x10008, cfg_dt_iso15765_pad_byte = 0x10000001, cfg_dt_half_duplex = 0x10000007,
};
// Where an SConfig parameter lives and what it may be set to, for one channel protocol (CAN or
// ISO15765). Built from the decompiled vendor getters and setters -- 1001ecb0/1001ef70 for ISO15765,
// 10018960/10018b30 for CAN, in analysis/decompiled/config/ -- and checked against a live firmware
// sweep (PROTOCOL.md section 7d). The vendor delegates to a base class for what a class does not own;
// the tables here are the flattened result.
struct ConfigRoute {
    // Firmware keeps most parameters and answers cGetValue / cSetValue on the channel node. LOOPBACK is
    // the exception: the vendor holds it in the channel object and never sends it to the firmware.
    enum class Where { Firmware, HostLoopback } where;
    uint32_t selector;       // firmware selector; unused for HostLoopback
    bool settable;           // false: readable, but this driver refuses to change it (see below)
    uint32_t minimum, maximum;  // accepted values for SET, inclusive
    bool allow_ffff;         // 0xFFFF also accepted (BS_TX, STMIN_TX: "no value")
};
// Throws Error(ERR_NOT_SUPPORTED) for a parameter the vendor rejects on this protocol, and
// Error(ERR_FAILED) for J1962_PINS, which the vendor only allows on the *_PS protocols.
ConfigRoute config_route(uint16_t protocol, uint32_t parameter);
// Throws Error(ERR_INVALID_IOCTL_VALUE) if the value is outside what the vendor's setter accepts, and
// Error(ERR_NOT_SUPPORTED) if this driver declines to set the parameter at all.
//
// Two parameters are readable but not settable here on purpose: DT_PULLUP_VALUE (an electrical pull-up
// on the CAN lines) and DT_HALF_DUPLEX. The vendor allows both; changing either on a live vehicle bus is
// an electrical act, and no capture shows it done. GET works for both.
void config_validate_set(uint16_t protocol, uint32_t parameter, uint32_t value);
// Whether a CAN bit rate is one the vendor accepts, for Connect and for DATA_RATE: the J2534 CAN rate list.
// Vendor predicate 10037890 has that list (18 rates) and a shorter one (125, 250 and 500 kbit) chosen by a field
// of the device object, +0xc240; PassThruOpen constructs the device with that field 0, so the long list applies
// (analysis/decompiled/baud/, PassThruOpen.c:265 and :1000).
bool can_baud_supported(uint32_t rate);
}
