#!/usr/bin/env python3
"""Walk the board state machine: FIRMWARE -> (reset) BOOTLOADER -> (jump) FIRMWARE.

`cResetBoard` restarts the adapter into the bootloader, which is why the vendor's
open path issues `cJumpToFirmware` first. Both transitions are non-destructive and
the adapter is left running firmware. The bootloader answers a reduced command set.

Re-enumeration takes a few seconds, so the tty is reopened between phases.
"""
import time

import _common
from mongoose_wire import Device, opcode_name

BOOTLOADER_PROBES = [0x0100, 0x0109, 0x0013, 0x0111, 0x0107, 0x0003]
RESET_BOARD = 0x0102
JUMP_TO_FIRMWARE = 0x0103
REENUMERATION_WAIT = 4.0


def phase(title):
    print("\n=== %s ===" % title)


def main():
    phase("baseline")
    with Device() as device:
        print("  %s" % device.board_info())
        print("\n  sending cResetBoard - adapter will re-enumerate")
        try:
            device.call(RESET_BOARD, wait=0.5)
        except OSError:
            pass  # the device drops the tty mid-reset

    time.sleep(REENUMERATION_WAIT)

    phase("after reset - expect BOOTLOADER")
    with Device() as device:
        info = device.board_info()
        print("  %s" % info)
        if info and info.status_name != "BOARDSTATUS_BOOTLOADER":
            print("  note: expected the bootloader here")
        print("\n  bootloader command set:")
        for opcode in BOOTLOADER_PROBES:
            messages, _ = device.call(opcode)
            _common.report("0x%04x %s" % (opcode, opcode_name(opcode)), messages)
        print("\n  restoring firmware with cJumpToFirmware")
        messages, _ = device.call(JUMP_TO_FIRMWARE, wait=1.5)
        _common.report("cJumpToFirmware", messages)

    time.sleep(REENUMERATION_WAIT)

    phase("after jump - expect FIRMWARE")
    with Device() as device:
        info = device.board_info()
        print("  %s" % info)
        return 0 if info and info.status_name == "BOARDSTATUS_FIRMWARE" else 1


if __name__ == "__main__":
    raise SystemExit(main())
