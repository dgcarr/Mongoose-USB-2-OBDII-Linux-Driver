#!/usr/bin/env python3
"""Validate the frame format against the adapter using read-only opcodes.

Exercises four different payload lengths; any resync slide would mean the
length/XOR header is wrong.
"""
import _common
from mongoose_wire import Device, build, opcode_name

PROBES = [0x0100, 0x0109, 0x0101, 0x0107]  # Echo, BoardInfo, BoardStatus, Stats


def main():
    with Device() as device:
        print("device: %s" % device.node)
        for opcode in PROBES:
            frame = build(opcode, device.sequence)
            print("\n>>> %s (0x%04x) seq=%d" % (opcode_name(opcode), opcode, device.sequence))
            print("    TX %s" % frame.hex())
            messages, slides = device.call(opcode)
            _common.report("RX", messages, slides)
            for message in messages:
                print("    %d body bytes, timestamp=%s us"
                      % (len(message.body), message.timestamp_us))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
