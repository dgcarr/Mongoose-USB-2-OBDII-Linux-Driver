"""Decode a USBPcap capture of the MongoosePro JLR adapter.

Reads a linktype 249 (LINKTYPE_USBPCAP) pcap, prints one line per USB transfer,
and applies the wire framing from PROTOCOL.md to every bulk payload:

    u16 length N | u16 (N XOR 0x51E6) | N body bytes
    body: u16 dst | u16 src | u16 opcode | u16 seq | u16 chan | u16 token
    response bodies continue: u32 status | u32 device microseconds | data

Usage:  python analysis/decode_usbpcap.py analysis/captures/windows/<run>/wire.pcap
"""
import struct, sys

TRANSFER = {0: 'ISOC', 1: 'INTR', 2: 'CTRL', 3: 'BULK', 0xfe: 'IRP_INFO'}
# USBPcap function codes worth naming
FUNC = {
    0x0008: 'ABORT_PIPE', 0x0009: 'GET_FRAME', 0x000b: 'CLASS_DEVICE',
    0x0017: 'CLASS_INTERFACE', 0x001a: 'VENDOR_DEVICE', 0x001b: 'VENDOR_INTERFACE',
    0x0000: 'SELECT_CONFIGURATION', 0x0001: 'SELECT_INTERFACE',
    0x0002: 'ABORT_PIPE2', 0x0015: 'CONTROL_TRANSFER',
    0x0009: 'BULK_OR_INTERRUPT_TRANSFER', 0x000009: 'BULK_OR_INTERRUPT',
    0x001e: 'CLASS_OTHER', 0x0028: 'CONTROL_TRANSFER_EX',
}

def decode(path):
    d = open(path, 'rb').read()
    off = 24
    n = 0
    while off + 16 <= len(d):
        ts, tus, incl, orig = struct.unpack_from('<IIII', d, off)
        off += 16
        pkt = d[off:off + incl]
        off += incl
        n += 1
        if len(pkt) < 27:
            print('pkt%-2d SHORT (%d bytes)' % (n, len(pkt)))
            continue
        hlen = struct.unpack_from('<H', pkt, 0)[0]
        irp = struct.unpack_from('<Q', pkt, 2)[0]
        status = struct.unpack_from('<I', pkt, 10)[0]
        func = struct.unpack_from('<H', pkt, 14)[0]
        info = pkt[16]
        bus = struct.unpack_from('<H', pkt, 17)[0]
        dev = struct.unpack_from('<H', pkt, 19)[0]
        ep = pkt[21]
        xfer = pkt[22]
        dlen = struct.unpack_from('<I', pkt, 23)[0]
        payload = pkt[hlen:]
        direction = 'IN <-' if (info & 1) else 'OUT->'
        extra = ''
        if xfer == 2 and hlen >= 28:          # control: stage byte at 27
            stage = pkt[27]
            stages = {0: 'SETUP', 1: 'DATA', 2: 'STATUS', 3: 'COMPLETE'}
            extra = ' stage=%s' % stages.get(stage, stage)
            if stage == 0 and hlen >= 35:
                bm, br, wv, wi, wl = struct.unpack_from('<BBHHH', pkt, 28)
                extra += ' setup=bmReq:%02x bReq:%02x wValue:%04x wIndex:%04x wLen:%d' % (bm, br, wv, wi, wl)
        print('pkt%-2d %.6f %-4s ep=0x%02x dev=%d %s func=0x%04x st=0x%08x len=%d%s' %
              (n, ts + tus / 1e6, TRANSFER.get(xfer, xfer), ep, dev, direction, func, status, dlen, extra))
        if payload:
            print('        data[%d] %s' % (len(payload), payload.hex()))
            frames(payload)

def frames(buf):
    """Repo framing: u16 length N | u16 (N XOR 0x51E6) | N body bytes."""
    off = 0
    while off + 4 <= len(buf):
        n = struct.unpack_from('<H', buf, off)[0]
        chk = struct.unpack_from('<H', buf, off + 2)[0]
        if (n ^ 0x51E6) & 0xFFFF != chk or not (1 <= n <= 0x1800):
            off += 1
            continue
        body = buf[off + 4: off + 4 + n]
        if len(body) < n:
            print('        !! truncated frame, want %d have %d' % (n, len(body)))
            return
        describe(body)
        off += 4 + n

OPCODES = {
    0x00: 'cPrintDebugText', 0x01: 'cRespGeneral', 0x03: 'cOpenDevice', 0x05: 'cCloseDevice',
    0x06: 'cOpenChannel', 0x07: 'cCloseChannel', 0x08: 'cOutboundData', 0x09: 'cInboundData',
    0x0a: 'cIndication', 0x0b: 'cSetValue', 0x0c: 'cGetValue', 0x11: 'cIoctl', 0x12: 'cSetPin',
    0x13: 'cGetString', 0x100: 'cEchoPacket', 0x101: 'cGetBoardStatus', 0x102: 'cResetBoard',
    0x103: 'cJumpToFirmware', 0x107: 'cGetStats', 0x109: 'cGetBoardInfo', 0x10a: 'cReflashBoard',
    0x10b: 'cWriteSerialNumber', 0x10c: 'cUnprotectBootloader', 0x111: 'cCheckCRN', 0x112: 'cUpdateBTModule',
}

def describe(body):
    if len(body) < 12:
        print('        FRAME short body %s' % body.hex())
        return
    dst, src, op, seq, chan, resv = struct.unpack_from('<HHHHHH', body, 0)
    name = OPCODES.get(op & 0x7fff, '?')
    resp = ' RESP' if op & 0x8000 else ''
    line = '        FRAME dst=%d src=%d op=0x%04x(%s%s) seq=%d chan=%d resv=%d' % (
        dst, src, op, name, resp, seq, chan, resv)
    rest = body[12:]
    if op & 0x8000 and len(rest) >= 8:
        status, micros = struct.unpack_from('<II', rest, 0)
        line += ' status=%d us=%d' % (status, micros)
        rest = rest[8:]
    print(line)
    if rest:
        txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in rest)
        print('          body[%d] %s  |%s|' % (len(rest), rest.hex(), txt))

for p in sys.argv[1:]:
    print('##### %s' % p)
    decode(p)
