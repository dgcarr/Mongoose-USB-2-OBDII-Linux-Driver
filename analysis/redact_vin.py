"""Redact a VIN from captured artefacts.

Mode 09 PID 02 returns the vehicle identification number, which identifies the
car, and captures are committed to a public repository. Replacements are the same
length as what they replace, so pcap structure and offsets are preserved.

ISO15765 splits the VIN across a first frame and consecutive frames with a PCI
byte between the pieces, so the whole string never appears contiguously in a wire
capture - only fragments do, and those still reassemble.

Matching is deliberately conservative. An earlier version replaced every
substring of three characters or more in both ASCII and hex form, which also
rewrites coincidental byte sequences in unrelated CAN data and silently corrupts
the capture. Here:

  - the full VIN is replaced wherever it appears, as ASCII or as hex text;
  - ASCII fragments are replaced only at MIN_ASCII_FRAGMENT characters or more,
    long enough that a collision in binary data is not a practical concern;
  - the short leading fragment is replaced only when anchored to the bytes that
    introduce it in a mode 09 PID 02 response, never on its own.

Usage:  python analysis/redact_vin.py <VIN> <file> [<file> ...]
"""
import sys

PLACEHOLDER = b'REDACTEDVIN000000'
MIN_ASCII_FRAGMENT = 5
# Service 0x49 (mode 09 response), PID 0x02, then the record-count byte.
ANCHORS = (b'\x49\x02\x01',)


def _swap(data, form, out):
    if form and form in data:
        return data.replace(form, out), data.count(form)
    return data, 0


def redact(path, vin):
    raw = vin.encode('ascii')
    filler = PLACEHOLDER[:len(raw)].ljust(len(raw), b'0')
    data = open(path, 'rb').read()
    hits = 0

    # 1. The whole VIN, as raw bytes and as hex text in either case.
    for form, out in ((raw, filler),
                      (raw.hex().encode(), filler.hex().encode()),
                      (raw.hex().upper().encode(), filler.hex().upper().encode())):
        data, n = _swap(data, form, out)
        hits += n

    # 2. ASCII fragments, longest first, down to MIN_ASCII_FRAGMENT.
    for length in range(len(raw), MIN_ASCII_FRAGMENT - 1, -1):
        for start in range(0, len(raw) - length + 1):
            data, n = _swap(data, raw[start:start + length], filler[start:start + length])
            hits += n

    # 3. The short leading fragment, only immediately after a response anchor.
    for anchor in ANCHORS:
        for length in range(MIN_ASCII_FRAGMENT - 1, 0, -1):
            data, n = _swap(data, anchor + raw[:length], anchor + filler[:length])
            hits += n

    if hits:
        open(path, 'wb').write(data)
    print('%-72s %d replacement(s)' % (path, hits))
    return hits


if len(sys.argv) < 3:
    print(__doc__)
    raise SystemExit(2)

vin = sys.argv[1]
total = sum(redact(p, vin) for p in sys.argv[2:])
print('total replacements: %d' % total)
