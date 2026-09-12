#!/usr/bin/env python3
"""Determine the cGetValue body shape, then sweep selectors.

The firmware reports `eInvalidMsgLength` for any body that is not exactly a
four-byte selector, which is what pins the format down.
"""
import struct

import _common
from mongoose_wire import Device

GET_VALUE = 0x000C
SHAPES = [
    ("u32 selector", struct.pack("<I", 1)),
    ("u16 selector", struct.pack("<H", 1)),
    ("u32 selector + u32", struct.pack("<II", 1, 0)),
]
SELECTORS = [1, 4, 5, 6, 7, 0x11, 0x16, 0x49, 0x100]


def main():
    with Device(first_sequence=200) as device:
        print("device: %s\n=== body shape ===" % device.node)
        for label, body in SHAPES:
            messages, _ = device.call(GET_VALUE, body)
            _common.report(label, messages)
        print("\n=== selector sweep (u32) ===")
        for selector in SELECTORS:
            messages, _ = device.call(GET_VALUE, struct.pack("<I", selector))
            _common.report("selector 0x%04x" % selector, messages)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
