#!/usr/bin/env python3
"""Prove vendor control request 0xdb is not a prerequisite.

`USBDEVFS_RESET` forces re-enumeration, clearing any firmware state an earlier
libusb session may have latched. The adapter is then driven purely through
cdc_acm with no control transfer of any kind.
"""
import fcntl
import os
import subprocess
import time

import _common
from mongoose_wire import Device, VENDOR_ID, opcode_name

USBDEVFS_RESET = ord("U") << 8 | 20
PROBES = [0x0100, 0x0109, 0x0013]
REENUMERATION_WAIT = 4.0


def usb_node(vendor_product="18e1:0104"):
    listing = subprocess.run(["lsusb", "-d", vendor_product],
                             capture_output=True, text=True).stdout.split()
    if not listing:
        return None
    return "/dev/bus/usb/%03d/%03d" % (int(listing[1]), int(listing[3].rstrip(":")))


def main():
    node = usb_node()
    if not node:
        print("adapter %s not present" % VENDOR_ID)
        return 1
    print("usb node: %s" % node)

    fd = os.open(node, os.O_WRONLY)
    try:
        fcntl.ioctl(fd, USBDEVFS_RESET, 0)
    finally:
        os.close(fd)
    print("USBDEVFS_RESET issued - any latched vendor state cleared")
    time.sleep(REENUMERATION_WAIT)

    with Device(first_sequence=7) as device:
        print("tty after reset: %s\n" % device.node)
        for opcode in PROBES:
            messages, slides = device.call(opcode, wait=1.0)
            _common.report("0x%04x %s" % (opcode, opcode_name(opcode)), messages, slides)
            if not messages:
                return 1
    print("\nAll answered with no control transfer sent.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
