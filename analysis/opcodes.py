#!/usr/bin/env python3
"""Recover the MongoosePro wire opcode map from monpj432.dll.

`FUN_1003b080` is a pure opcode->name lookup compiled by MSVC into three tiers:
dense ranges use a direct dword jump table, and the sparse 0x8005..0x8103 range
uses a byte index table feeding a smaller address table. Every case is a
`B8 <imm32> C3` thunk (`mov eax, <wide string>; ret`), so the opcode for each
case is recovered by walking the tables rather than reading 61 cases by hand.

Usage:  python3 analysis/opcodes.py [path/to/monpj432.dll]
"""
import struct
import sys

DEFAULT_DLL = "vendor/driver/monpj432.dll"

# Dispatch geometry of FUN_1003b080, read off the disassembly.
_DEFAULT_CASE = 0x1003B26C
_DIRECT_TABLES = (
    (0x00, 0x16, 0x1003B274),   # opcodes 0x00..0x15
    (0x101, 0x12, 0x1003B2CC),  # opcodes 0x101..0x112
    (0x810A, 0x09, 0x1003B458),  # opcodes 0x810a..0x8112
)
_INDEXED_TABLE = (0x8005, 0xFF, 0x1003B358, 0x1003B314)  # base, count, byte idx, addrs
_SINGLETONS = ((0x100, 0x1003B128), (0x8003, 0x1003B1A6), (0x8109, 0x1003B237))


class _Image:
    """Flat virtual-address view of a PE."""

    def __init__(self, path):
        import pefile

        pe = pefile.PE(path)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.data = pe.get_memory_mapped_image()

    def u8(self, va):
        return self.data[va - self.base]

    def u32(self, va):
        return struct.unpack_from("<I", self.data, va - self.base)[0]

    def wide_string(self, va):
        off = va - self.base
        end = self.data.index(b"\x00\x00", off)
        if (end - off) % 2:
            end += 1
        return self.data[off:end].decode("utf-16-le")


def extract_opcodes(dll_path=DEFAULT_DLL):
    """Return {opcode: name} for every case of the opcode->name switch."""
    img = _Image(dll_path)

    def thunk_name(case_va):
        if img.u8(case_va) != 0xB8:  # mov eax, imm32
            return None
        return img.wide_string(img.u32(case_va + 1))

    opcodes = {}

    def record(opcode, case_va):
        if case_va == _DEFAULT_CASE:
            return
        name = thunk_name(case_va)
        if name:
            opcodes[opcode] = name

    for first, count, table in _DIRECT_TABLES:
        for i in range(count):
            record(first + i, img.u32(table + 4 * i))
    first, count, index_table, address_table = _INDEXED_TABLE
    for i in range(count):
        record(first + i, img.u32(address_table + 4 * img.u8(index_table + i)))
    for opcode, case_va in _SINGLETONS:
        record(opcode, case_va)
    return opcodes


def response_of(opcode):
    """Responses are the command opcode with bit 15 set."""
    return opcode | 0x8000


def check_pairing(opcodes):
    """Verify every *Resp name is its command's opcode | 0x8000.

    Returns a list of (opcode, name, expected_command_name, actual) mismatches.
    """
    bad = []
    for opcode, name in sorted(opcodes.items()):
        if not name.endswith("Resp"):
            continue
        command = opcodes.get(opcode & ~0x8000)
        if command != name[:-4]:
            bad.append((opcode, name, name[:-4], command))
    return bad


def main(argv):
    dll = argv[1] if len(argv) > 1 else DEFAULT_DLL
    opcodes = extract_opcodes(dll)
    print("# MongoosePro wire opcodes (monpj432.dll FUN_1003b080)")
    for opcode in sorted(opcodes):
        print("0x%04x   %s" % (opcode, opcodes[opcode]))
    print("\ntotal: %d" % len(opcodes))
    mismatches = check_pairing(opcodes)
    print("response = command | 0x8000 mismatches: %d" % len(mismatches))
    for entry in mismatches:
        print("  0x%04x %s expected command %s, found %s" % entry)
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
