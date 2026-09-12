#!/usr/bin/env python3
"""MongoosePro JLR wire protocol over plain cdc_acm.

Frame:    u16 length | u16 length ^ 0x51E6 | payload[length]
Message:  u16 dst | u16 src | u16 opcode | u16 seq | u16 chan | u16 reserved | body
Response: body+0 u32 status | body+4 u32 device microseconds | body+8 command data

The adapter enumerates as a standard CDC-ACM device, so this needs no libusb, no
interface detach and no vendor control transfer - only membership of `dialout`.
"""
import glob
import os
import select
import struct
import termios
import time

from wire_tables import BOARD_STATUS, OPCODES, STATUS

KEY = 0x51E6
MAX_PAYLOAD = 0x1800
HEADER_LEN = 4
MESSAGE_HEADER_LEN = 12
VENDOR_ID = "18e1"

PC_NODE = 0
BOARD_NODE = 1

# Commands that write the adapter's own flash or identity. A vehicle being absent
# does not make these safe: they are excluded from every probe in this directory.
DESTRUCTIVE = {
    0x0104: "cSetBoardID",
    0x010A: "cReflashBoard",
    0x010B: "cWriteSerialNumber",
    0x010C: "cUnprotectBootloader",
    0x0112: "cUpdateBTModule",
}


def opcode_name(opcode):
    """Name an opcode, deriving unnamed responses from `command | 0x8000`."""
    if opcode in OPCODES:
        return OPCODES[opcode]
    command = opcode & ~0x8000
    if opcode & 0x8000 and command in OPCODES:
        return OPCODES[command] + "Resp"
    return "op_0x%04x" % opcode


def status_name(status):
    return STATUS.get(status, "0x%04x" % status)


def find_tty(vendor_id=VENDOR_ID):
    """Locate the /dev/ttyACM* backed by the adapter rather than a fixed number."""
    for node in sorted(glob.glob("/dev/ttyACM*")):
        path = os.path.realpath("/sys/class/tty/%s/device" % os.path.basename(node))
        for _ in range(4):
            path = os.path.dirname(path)
            marker = os.path.join(path, "idVendor")
            if os.path.exists(marker):
                if open(marker).read().strip() == vendor_id:
                    return node
                break
    return None


def build(opcode, sequence, body=b"", dst=BOARD_NODE, src=PC_NODE, channel=0):
    payload = struct.pack("<HHHHHH", dst, src, opcode, sequence, channel, 0) + body
    return struct.pack("<HH", len(payload), len(payload) ^ KEY) + payload


def deframe(buffer):
    """Split a byte stream into payloads, mirroring the DLL's resync behaviour.

    Returns (payloads, resync_slides, remainder). An invalid header advances by a
    single byte, exactly as FUN_1006b1c0 does, so the stream is self-synchronising.
    """
    payloads, offset, slides = [], 0, 0
    while offset + HEADER_LEN <= len(buffer):
        length, check = struct.unpack_from("<HH", buffer, offset)
        if length == 0 or length > MAX_PAYLOAD or (check ^ KEY) != length:
            offset += 1
            slides += 1
            continue
        if offset + HEADER_LEN + length > len(buffer):
            break
        payloads.append(buffer[offset + HEADER_LEN:offset + HEADER_LEN + length])
        offset += HEADER_LEN + length
    return payloads, slides, buffer[offset:]


class Message:
    """One parsed payload."""

    def __init__(self, payload):
        self.raw = payload
        (self.dst, self.src, self.opcode, self.sequence,
         self.channel, self.reserved) = struct.unpack_from("<HHHHHH", payload, 0)
        self.body = payload[MESSAGE_HEADER_LEN:]

    @property
    def name(self):
        return opcode_name(self.opcode)

    @property
    def status(self):
        if len(self.body) < 4:
            return None
        return struct.unpack_from("<I", self.body, 0)[0]

    @property
    def timestamp_us(self):
        """Device microsecond counter; cOpenDevice resets it to zero."""
        if len(self.body) < 8:
            return None
        return struct.unpack_from("<I", self.body, 4)[0]

    @property
    def data(self):
        """Command-specific bytes after status and timestamp."""
        return self.body[8:] if len(self.body) > 8 else b""

    @property
    def text(self):
        """Failed commands carry NUL-terminated ASCII here."""
        data = self.data
        if data[:1].isalpha():
            return data.split(b"\0")[0].decode("latin1", "replace")
        return ""

    def __str__(self):
        status = self.status
        detail = '"%s"' % self.text if self.text else self.data[:24].hex()
        return "0x%04x %-24s status=%-22s %s" % (
            self.opcode, self.name,
            status_name(status) if status is not None else "-", detail)


class BoardInfo:
    """The 28-byte header shared by cGetBoardStatus and cGetBoardInfo bodies."""

    def __init__(self, data):
        self.board_type = data[0]
        self.status = data[1]
        self.bootloader = struct.unpack_from("<I", data, 12)[0]
        self.firmware = struct.unpack_from("<I", data, 16)[0]

    @staticmethod
    def _version(value):
        return "%d.%d.%d.%d" % ((value >> 24) & 0xFF, (value >> 16) & 0xFF,
                                (value >> 8) & 0xFF, value & 0xFF)

    @property
    def status_name(self):
        return BOARD_STATUS.get(self.status, "?")

    def __str__(self):
        return "type=%d status=%d(%s) bootloader=%s firmware=%s" % (
            self.board_type, self.status, self.status_name,
            self._version(self.bootloader), self._version(self.firmware))


class Device:
    """A command/response session over the adapter's CDC-ACM tty."""

    def __init__(self, node=None, first_sequence=1):
        self.node = node or find_tty()
        if not self.node:
            raise RuntimeError("no MongoosePro tty found (VID %s)" % VENDOR_ID)
        self.fd = None
        self.sequence = first_sequence

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *_):
        self.close()

    def open(self):
        self.fd = os.open(self.node, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0                                              # iflag: no processing
        attrs[1] = 0                                              # oflag
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0                                              # lflag: raw
        attrs[4] = attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)
        self.drain(0.3)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def drain(self, seconds):
        """Collect everything the adapter sends within a fixed window."""
        buffer = b""
        deadline = time.time() + seconds
        while time.time() < deadline:
            readable, _, _ = select.select([self.fd], [], [], 0.05)
            if not readable:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if chunk:
                buffer += chunk
        return buffer

    def call(self, opcode, body=b"", wait=0.8, allow_destructive=False):
        """Send one command and return (messages, resync_slides)."""
        if opcode in DESTRUCTIVE and not allow_destructive:
            raise ValueError(
                "%s (0x%04x) writes adapter flash or identity; "
                "pass allow_destructive=True to override"
                % (DESTRUCTIVE[opcode], opcode))
        os.write(self.fd, build(opcode, self.sequence, body))
        self.sequence += 1
        payloads, slides, _ = deframe(self.drain(wait))
        return [Message(p) for p in payloads], slides

    def board_info(self):
        """Read the board header, or None if the adapter did not answer."""
        messages, _ = self.call(0x0101)
        for message in messages:
            if len(message.data) >= 20:
                return BoardInfo(message.data)
        return None
