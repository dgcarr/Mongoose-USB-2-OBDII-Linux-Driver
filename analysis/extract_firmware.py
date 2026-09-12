#!/usr/bin/env python3
"""Extract the firmware and bootloader images embedded in monpj432.dll.

They are RT_RCDATA resources 5005 (firmware) and 5006 (bootloader). Header:
    u32 header_size (0x6c) | u16 image_type (1=bootloader, 2=firmware)
    u16 board_type | u32 version | char name[] (NUL padded)
The payload after header_size bytes is high-entropy (compressed or encrypted).
"""
import pefile, struct, sys, os

DLL = sys.argv[1] if len(sys.argv) > 1 else 'vendor/driver/monpj432.dll'
OUT = sys.argv[2] if len(sys.argv) > 2 else '.'

pe = pefile.PE(DLL)
for t in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    if pefile.RESOURCE_TYPE.get(t.struct.Id, '') != 'RT_RCDATA':
        continue
    for nm in t.directory.entries:
        for lang in nm.directory.entries:
            d = lang.data.struct
            data = pe.get_data(d.OffsetToData, d.Size)
            hdr, itype, btype, ver = struct.unpack_from('<IHHI', data, 0)
            name = data[12:hdr].split(b'\0')[0].decode('latin1')
            path = os.path.join(OUT, 'fw_%d.bin' % nm.struct.Id)
            open(path, 'wb').write(data)
            print("%s  %7d B  type=%d board=%d  version=%d.%d.%d.%d  %r"
                  % (path, len(data), itype, btype,
                     (ver >> 24) & 255, (ver >> 16) & 255, (ver >> 8) & 255, ver & 255, name))
