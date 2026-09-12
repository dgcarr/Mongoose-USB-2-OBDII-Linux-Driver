# MongoosePro JLR — research status

Updated 2026-09-13. The current technical reference is [PROTOCOL.md](PROTOCOL.md).
Implementation update: Linux discovery, open/close, firmware version and voltage
queries now succeed on the USB-only adapter. See [current validation](docs/VALIDATION.md)
and `analysis/captures/linux-*.trace`. The research summary below predates these
hardware runs. Vehicle communication is not implemented; the next reference setup
is a Windows laptop and a 2017 Volvo XC60 D5 AWD.
The chronological research log, including superseded hypotheses, is preserved in
[the history archive](analysis/history/NOTES-before-consolidation.md).

## Current findings

- Device: MongoosePro JLR, USB VID:PID `18e1:0104`, enumerating as CDC-ACM on Linux.
- The Windows kernel driver sends vendor control request `40 db 01 00 00 00 00 00`
  on file creation, and the same request with value zero on file cleanup. **It is not a
  prerequisite**: after a `USBDEVFS_RESET` the adapter answers Echo, GetBoardInfo and
  GetString over plain `/dev/ttyACM*` with no control transfer at all, so a Linux client
  needs neither libusb nor interface detachment (PROTOCOL.md section 7a).
- Bulk reads and writes forward application buffers without additional framing changes.
- Wire framing is `LE16(N) | LE16(N XOR 0x51e6) | N body bytes`.
- Internal response queues store `LE32(N) | body | LE32(N)`. Those length words are not
  transmitted framing or checksums. Correcting register signatures resolved the earlier
  decompiler uncertainty without requiring interactive GUI tracing.
- Response matching compares routing and sequence fields, not opcode. The dispatcher
  filters response types before they enter the queue.
- Echo (`0x100`) is answered with opcode `0x8100` and a 12-byte body, shorter than the
  20-byte general response. The vendor opcode→name table has no entry for `0x8100`, but
  the response is present in `analysis/captures/linux-discovery-20260912T2030.trace`, so
  the table is incomplete rather than the opcode invalid. `0x8101` and `0x8107` are
  likewise absent from the table and likewise observed on the wire; all three were
  predicted by `request | 0x8000` before being seen.
- The observed open path sends StartFirmware, OpenDevice, then GetBoardInfo. A separate
  discovery path sends Echo and GetBoardInfo before opening a diagnostic session.
- Open response status `0x020a` explicitly reports missing vehicle-connector voltage.
- Normal message layouts, configuration requests, filters and periodic requests are
  partially mapped. ISO15765 receive reassembly includes host-side DLL logic.

## Validation boundary

The earlier Linux capture confirmed bytes reach bulk OUT unchanged, but recorded no
application responses. An independent pass on 2026-09-13 re-derived the framing, opcode
map and enumerations from the binaries alone and then confirmed them against the adapter
over `cdc_acm` (read-only opcodes only, no vehicle): see PROTOCOL.md section 7a and
`analysis/probes/`. Findings not marked as hardware-checked there remain static evidence.

Unknowns include channel allocation and open arguments, pin routing, header bytes 10–11,
status masks, and complete ISO15765 transmit/flow-control behavior. Timestamp units are
resolved: response+16 is a device microsecond counter zeroed by `cOpenDevice`.
The old probe script (`analysis/history/probe.py`) sweeps guessed frames and does not
implement the corrected protocol or vendor initialization; it is a historical experiment,
not a current client. The archived notes under `analysis/history/` still refer to it at
its original `analysis/probe.py` path, which is left intact so the record stays faithful.

- `cResetBoard` restarts into the bootloader; `cJumpToFirmware` returns to firmware. The
  bootloader runs a reduced command set and reports `eNotSupported` where firmware reports
  `eFailed`. Both images ship inside `monpj432.dll` as RT_RCDATA 5005/5006 at exactly the
  versions the adapter runs (`analysis/extract_firmware.py`).
- `FUN_10037c10` is the complete wire status/indication space (66 codes), which names
  `0x020a eVbattLoss` and the `cIndication` code band (`0x106 iMsgTxDone`).

## Research milestones

| Date | Result |
|---|---|
| 2026-09-12 | Extracted command names and exports; captured unsuccessful Linux probes |
| 2026-09-12 | Examined FIFO helpers; initial wire-header interpretation later disproved |
| 2026-09-13 | Traced control construction through WriteFile; recovered length-XOR framing |
| 2026-09-13 | Used Ghidra to trace startup, 47 control senders and 55 kernel functions |
| 2026-09-13 | Recovered vendor USB initialization, queue layout and message/ISO15765 handling |
| 2026-09-13 | Independent pass: machine-extracted enumerations; framing, routing, opcode rule and microsecond timestamps confirmed on hardware over plain cdc_acm |

## Next work

1. Validate the vendor-create request and discovery Echo/GetBoardInfo sequence in a
   narrowly scoped USB capture using a corrected probe.
2. Trace channel-ID allocation, open-channel arguments and pin configuration.
3. Finish protocol-specific message/status and ISO15765 handling before claiming a
   complete Linux J2534 implementation.

See [reproduction instructions](analysis/REPRODUCE.md) and the
[sender index](analysis/SENDERS.md) for the saved evidence and Ghidra scripts.
