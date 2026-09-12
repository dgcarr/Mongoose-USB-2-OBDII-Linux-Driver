#!/usr/bin/env python3
"""Print the adapter's board header. Also the liveness check after a reset."""
import _common  # noqa: F401  (sets sys.path)
from mongoose_wire import Device


def main():
    with Device() as device:
        print("device: %s" % device.node)
        info = device.board_info()
        if not info:
            print("NO RESPONSE - adapter did not answer")
            return 1
        print("ALIVE: %s" % info)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
