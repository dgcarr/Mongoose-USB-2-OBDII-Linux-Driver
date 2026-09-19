# Hardware probe scripts

Independent validation of the recovered wire protocol against the adapter over plain
`cdc_acm` (`/dev/ttyACM*`, located by USB vendor ID). Requires `dialout` membership.
**No libusb, no interface detach, no vendor control transfer.**

| Script | Purpose |
|---|---|
| `mongoose_wire.py` | The protocol: framing, message parsing, `Device` session, board header |
| `wire_tables.py` | Generated opcode/status names — `python3 analysis/gen_wire_tables.py` |
| `board_status.py` | One-shot board header; the liveness check after a reset |
| `probe_framing.py` | Frame format over four payload lengths; any resync slide means a wrong header |
| `probe_session.py` | Session opcodes, showing the firmware's own error text |
| `probe_getvalue.py` | Pins the `cGetValue` body shape, then sweeps selectors |
| `probe_boardstate.py` | FIRMWARE → (reset) BOOTLOADER → (jump) FIRMWARE; leaves firmware running |
| `reset_test.py` | `USBDEVFS_RESET`, then re-probes — proves `0xdb` is not required |
| `write_batch_probe.c` | Through the J2534 library: single versus multi-message queued writes (C; build line in its header) |

## Safety

`mongoose_wire.Device.call()` refuses the opcodes that write the adapter's own flash or
identity — `cSetBoardID`, `cReflashBoard`, `cWriteSerialNumber`, `cUnprotectBootloader`,
`cUpdateBTModule` — unless `allow_destructive=True` is passed. **A vehicle being absent does
not make these safe**: they are writes to the adapter, not to the bus. `cUnprotectBootloader`
in particular has no software recovery path if the bootloader is corrupted.

`cResetBoard` and `cJumpToFirmware` *are* permitted: they only move the adapter between
firmware and bootloader, and `probe_boardstate.py` demonstrates the round trip.

## Wire format

    Frame:    u16 length | u16 length ^ 0x51E6 | payload[length]
    Message:  u16 dst | u16 src | u16 opcode | u16 seq | u16 chan | u16 reserved | body
    Response: body+0 u32 status | body+4 u32 device microseconds | body+8 command data

`dst`/`src` are node ids: PC is 0, boards are 1–8. Response opcode is `command | 0x8000`.
