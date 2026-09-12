#!/usr/bin/env python3
"""Send hypothesis frames to the MongoosePro JLR adapter over /dev/ttyACM0 and
print whatever comes back, for empirical validation against analysis/PROTOCOL.md."""
import os
import sys
import time
import select
import struct
import termios
import tty

DEV = "/dev/ttyACM0"

OPCODES = {
    "cOpenDevice": 0x003,
    "cEchoPacket": 0x100,
    "cGetBoardStatus": 0x101,
    "cGetBoardInfo": 0x109,
    "cGetDeviceConfiguration": 0x004,
}


def open_raw(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    tty.setraw(fd)
    # keep it blocking-friendly: VMIN=0, VTIME=0, we'll poll with select
    attrs = termios.tcgetattr(fd)
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def build_frame(opcode, seq, endian, len_includes_self, pad_to=12, payload=b""):
    fmt = "<" if endian == "little" else ">"
    body = struct.pack(fmt + "HH", opcode, seq) + payload
    if len(body) + 4 < pad_to:
        body += b"\x00" * (pad_to - 4 - len(body))
    total_len = len(body) + 4 if len_includes_self else len(body)
    length_field = struct.pack(fmt + "I", total_len)
    return length_field + body


def read_available(fd, timeout=0.5):
    end = time.time() + timeout
    chunks = []
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            data = os.read(fd, 4096)
            if data:
                chunks.append(data)
                end = time.time() + 0.2  # extend a bit if data is still trickling in
    return b"".join(chunks)


def main():
    fd = open_raw(DEV)
    print(f"Opened {DEV} fd={fd}")

    seq = 1
    trials = []
    for opname, op in OPCODES.items():
        for endian in ("little", "big"):
            for len_incl in (True, False):
                trials.append((opname, op, endian, len_incl))

    for opname, op, endian, len_incl in trials:
        frame = build_frame(op, seq, endian, len_incl)
        seq += 1
        # drain any stale bytes first
        stale = read_available(fd, 0.05)
        if stale:
            print(f"  [drained stale bytes before send]: {stale.hex()}")
        print(f"--> {opname} (0x{op:x}) endian={endian} len_includes_self={len_incl} "
              f"frame={frame.hex()}")
        os.write(fd, frame)
        resp = read_available(fd, 1.2)
        if resp:
            print(f"<-- {len(resp)} bytes: {resp.hex()}")
        else:
            print("<-- (no response)")
        time.sleep(0.2)

    os.close(fd)


if __name__ == "__main__":
    main()
