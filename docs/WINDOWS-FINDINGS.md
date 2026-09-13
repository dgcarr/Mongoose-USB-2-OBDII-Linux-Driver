# Windows reference captures

First Windows-side results for this project. Every claim below cites the capture
that proves it; decode any of them with:

```sh
python analysis/decode_usbpcap.py analysis/captures/windows/<run>/wire.pcap
```

Environment: Windows 11 Home 10.0.26200, vendor package `J2534 MongoosePro JLR`
01.01.16.00. The installed `monpj432.dll` is **byte-identical** to
`vendor/driver/monpj432.dll` (sha256 `2710726f…9413`), and `dtmonpro.sys` matches
`vendor/driver/dtmonpro.sys` (sha256 `c3a5b6db…2b4a`), so the existing Ghidra
analysis applies to the capture machine unchanged. Adapter serial
`AOLHE0000003666A`, firmware 1.1.16.0.

## B1 — PassThruOpen / PassThruClose

Capture: `analysis/captures/windows/20260913T160645-b1-open-close/` (42 packets).
Stimulus: `tools/scripts/b1-open-close.txt`, `--gap 2000`. The API log records
`Open result=0 device=1` and `Close result=0`.

The vendor's `PassThruOpen` is eight wire commands, not one:

| seq | opcode | name | result |
|----|--------|------|--------|
| 1 | `0x0100` | cEchoPacket | status 0 |
| 2 | `0x0109` | cGetBoardInfo | 184-byte response |
| 3 | `0x000c` | cGetValue, selector `0x2f` | value 1 |
| 4 | `0x0111` | cCheckCRN | status 0 |
| 5 | `0x0103` | cJumpToFirmware | **status 7**, `Board already in firmware` |
| 6 | `0x0003` | cOpenDevice | status 0, **µs = 0** |
| 7 | `0x0109` | cGetBoardInfo | µs = 400 |

`PassThruClose` is one command, `0x0005 cCloseDevice` (seq 8), returning
µs = 2 003 000.

### Confirmed

- **`cJumpToFirmware` precedes `cOpenDevice`.** `PROTOCOL.md` inferred this from
  the bootloader behaviour; the vendor really does issue it every open, and
  really does ignore the resulting failure. The returned status 7 and the string
  `Board already in firmware` match `docs/VALIDATION.md` exactly.
- **`cOpenDevice` resets the device microsecond counter.** Its own response
  carries µs = 0, the next command reads 400, and the close 2 003 000 against an
  API-level gap of 2 000 ms. This is the first end-to-end confirmation that the
  counter is the origin of J2534 timestamps, and it cross-checks the wire against
  the API log independently.
- **Node IDs.** Every request is `dst=1 src=0`, every response `dst=0 src=1`.
  PC = 0, board = 1, as documented.
- **`0x8100` is not a mystery opcode.** It is simply `0x0100 | 0x8000`, the
  `cEchoPacket` response. `PROTOCOL.md:274` warned against "correcting" the
  decoder to the vendor name table; that warning was right, and the reason is now
  visible rather than inferred. The same applies to `0x8109`, `0x8111`, `0x800c`.
- **Vendor control request `0xdb` is real on Windows.** `bmRequestType 0x40`,
  `bRequest 0xdb`, `wValue 1` to enable and `0` to disable. The Linux work proved
  it is not a *prerequisite*; this shows the vendor sends it anyway.

### New

- **`cGetValue` selector `0x2f`** (47) is part of the open sequence and returns
  `1` on this unit. Previously only `0x2b` (firmware), `0x2a` (bootloader), `3`
  (battery) and `2` (programming voltage) were known. Meaning not yet
  established — one sample, one unit.
- **`cCheckCRN` (`0x111`) is a normal open-path command**, not an exotic one.
- **Body offset +10 is not "reserved".** It is a 16-bit value the firmware
  **echoes verbatim** in the response. Observed across one run: `0x00e4`,
  `0x7719`, `0x0000`, `0x0000`, `0x7719`, `0x008f`, `0xffff`, `0x0000` — including
  two different values for the same opcode (`cGetBoardInfo` at seq 2 and seq 7).
  That is consistent with `PROTOCOL.md`'s observation that the vendor builders
  leave the field with no explicit initializer: it reads as uninitialised caller
  memory, and the firmware ignores it and echoes it back. So the field is neither
  reserved-must-be-zero nor meaningful; treat it as an opaque echoed token.
- **Control-transfer preamble.** The open begins with `0xdb` `wValue=1`, then
  `wValue=0`, then `wValue=1` again before any bulk traffic — three transfers, not
  one. Not yet explained.
- **`cGetBoardInfo` body is stable across the open.** Both responses carry the
  same 160-byte body beginning `05 01 01 01 b4 b0 00 00 d8 55 00 00 00 08 01 01
  00 10 01 01`, matching the board type 5 / `0xB0B4` / `0x55D8` / bootloader
  1.1.8.0 / firmware 1.1.16.0 decode in `PROTOCOL.md:440-448`.

### Not yet established

The `0x2f` selector's meaning, the three-transfer `0xdb` preamble, and whether
the echoed +10 token is ever read by firmware. No channel was opened, so nothing
here touches the channel layer.

## Capture procedure notes

Two practical findings that cost real time, recorded so they are not rediscovered:

- **USBPcap captures nothing without `-A`** (or an explicit `--devices` list). With
  no device selection it prints `Selected capture options result in empty capture`
  to stderr and exits **0**, leaving either no file or a bare 24-byte pcap header.
  An exit status of 0 is not evidence that a capture happened.
- **USBPcap needs several seconds to attach.** At `-SettleMs 1500` the first run
  recorded only the close and missed the whole open sequence, which would have
  been easy to mistake for "the vendor does nothing on open". `capture.ps1`
  defaults to a longer settle; treat a capture whose first packet is not a control
  transfer as suspect.
- Capture works **without elevation**. `--extcap-interfaces` prints nothing when
  USBPcapCMD is invoked from a script file, so `capture.ps1` probes one control
  device per root hub instead and caches the one that saw traffic.
