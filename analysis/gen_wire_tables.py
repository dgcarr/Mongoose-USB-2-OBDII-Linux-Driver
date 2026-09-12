#!/usr/bin/env python3
"""Regenerate analysis/probes/wire_tables.py from the vendor binaries.

The probe scripts need opcode and status names at runtime but must not depend on
Ghidra or pefile, so the tables are generated once and committed. Re-run this
after re-extracting the DLL:

    python3 analysis/gen_wire_tables.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from enums import extract_enums  # noqa: E402
from opcodes import extract_opcodes  # noqa: E402

OUT = os.path.join(HERE, "probes", "wire_tables.py")
HEADER = '''"""Opcode and status names recovered from monpj432.dll.

GENERATED FILE - do not edit by hand.
Regenerate with:  python3 analysis/gen_wire_tables.py
"""

'''


def _emit(handle, name, values, comment):
    handle.write("# %s\n%s = {\n" % (comment, name))
    for identifier in sorted(values):
        handle.write("    0x%04x: %r,\n" % (identifier, values[identifier]))
    handle.write("}\n\n")


def main():
    opcodes = extract_opcodes()
    tables = extract_enums()
    with open(OUT, "w") as handle:
        handle.write(HEADER)
        _emit(handle, "OPCODES", opcodes, "FUN_1003b080 - response opcode = command | 0x8000")
        _emit(handle, "STATUS", tables["Wire status / indication codes"][1],
              "FUN_10037c10 - response body+12, and cIndication body+12")
        _emit(handle, "BOARD_STATUS", tables["Board status"][1], "FUN_1003b6e0 - board header body+9")
    print("wrote %s (%d opcodes, %d status codes)"
          % (OUT, len(opcodes), len(tables["Wire status / indication codes"][1])))


if __name__ == "__main__":
    main()
