#!/usr/bin/env python3
"""Recover the named enumerations from monpj432.dll's name-lookup switches.

Each of these functions is a compiled `switch` returning a wide string literal.
Ghidra recovers most of them as dense `case <id>: return L"NAME";` chains, so the
values can be parsed straight out of the decompiled C. `FUN_1003a2c0` is the
exception: MSVC compressed it into a byte index table, so its `case` labels are
indices and must be inverted through that table in the PE image.

Usage:  python3 analysis/enums.py [decompiled_dir] [path/to/monpj432.dll]
"""
import os
import re
import sys

DEFAULT_DECOMP = "analysis/decompiled"
DEFAULT_DLL = "vendor/driver/monpj432.dll"

# address -> (title, ordering key). Direct switches; values are the real IDs.
DIRECT_TABLES = {
    "10038b70": "SConfig parameter IDs",
    "10038020": "IOCTL IDs",
    "10037a90": "J2534 error codes",
    "100382f0": "Protocol IDs",
    "1003ae40": "Channel capability IDs",
    "1003b6e0": "Board status",
    "10037c10": "Wire status / indication codes",
}
# address -> (title, byte-index table VA, first id, id count)
INDEXED_TABLES = {
    "1003a2c0": ("Device capability IDs", 0x1003A608 + 3, 1, 0x100),
}

_CASE_ONLY = re.compile(r"\s*case (0x[0-9a-f]+|\d+):\s*$")
_CASE_RETURN = re.compile(r'\s*case (0x[0-9a-f]+|\d+):\s*return L"([^"]+)";')
_RETURN = re.compile(r'\s*return L"([^"]+)";')
# Some switches assign the name instead of returning it (`pwVar1 = L"NAME"; break;`).
_ASSIGN = re.compile(r'\s*\w+ = L"([^"]+)";')
_BLOCK_END = re.compile(r"\s*(if|switch|\})")


def _int(text):
    return int(text, 16 if text.startswith("0x") else 10)


def _parse_cases(path):
    """Parse `case N: return L"NAME";`, including cases that share one return."""
    values = {}
    pending = []
    with open(path) as handle:
        for line in handle:
            match = _CASE_ONLY.match(line)
            if match:
                pending.append(_int(match.group(1)))
                continue
            match = _CASE_RETURN.match(line)
            if match:
                values[_int(match.group(1))] = match.group(2)
                pending = []
                continue
            match = _RETURN.match(line) or _ASSIGN.match(line)
            if match and pending:
                for value in pending:
                    values[value] = match.group(1)
                pending = []
                continue
            if _BLOCK_END.match(line):
                pending = []
    return values


def _decomp_path(decomp_dir, address):
    """The repo and a fresh Ghidra dump name these files differently."""
    for name in ("strref_FUN_%s_%s.c" % (address, address),
                 "FUN_%s_%s.c" % (address, address),
                 "byaddr_%s_FUN_%s.c" % (address, address)):
        path = os.path.join(decomp_dir, name)
        if os.path.exists(path):
            return path
    return None


def extract_enums(decomp_dir=DEFAULT_DECOMP, dll_path=DEFAULT_DLL):
    """Return {title: {id: name}} for every recoverable enumeration."""
    tables = {}
    for address, title in DIRECT_TABLES.items():
        path = _decomp_path(decomp_dir, address)
        if path:
            tables[title] = (address, _parse_cases(path))

    for address, (title, index_va, first, count) in INDEXED_TABLES.items():
        path = _decomp_path(decomp_dir, address)
        if not path:
            continue
        from opcodes import _Image  # same-directory helper

        img = _Image(dll_path)
        by_index = _parse_cases(path)
        values = {}
        for identifier in range(first, first + count):
            name = by_index.get(img.u8(index_va + identifier))
            if name:
                values[identifier] = name
        tables[title] = (address, values)
    return tables


def main(argv):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    decomp = argv[1] if len(argv) > 1 else DEFAULT_DECOMP
    dll = argv[2] if len(argv) > 2 else DEFAULT_DLL
    for title, (address, values) in extract_enums(decomp, dll).items():
        print("\n########## %s (FUN_%s) : %d entries ##########" % (title, address, len(values)))
        for identifier in sorted(values):
            print("  0x%08x  %s" % (identifier, values[identifier]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
