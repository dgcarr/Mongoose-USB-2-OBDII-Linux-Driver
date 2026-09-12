#!/usr/bin/env python3
"""Probe session-level opcodes and show the firmware's own error text."""
import _common
from mongoose_wire import Device, opcode_name

PROBES = [0x0003, 0x0004, 0x000C, 0x0013, 0x0015, 0x0111, 0x0107]


def main():
    with Device(first_sequence=100) as device:
        print("device: %s" % device.node)
        for opcode in PROBES:
            messages, slides = device.call(opcode)
            _common.report("0x%04x %s" % (opcode, opcode_name(opcode)), messages, slides)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
