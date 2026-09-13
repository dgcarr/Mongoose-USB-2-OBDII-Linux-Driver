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

## C — The channel layer

Captures: `20260913T161048-c1-connect-can-500k`, `…-c2-proto-ISO15765`,
`…-c2-baud-250000`, `…-c2-flags-29bit`. Vehicle: 2017 Volvo XC60 D5 AWD, engine
running. All four `PassThruConnect` calls returned `STATUS_NOERROR`.

This closes the project's largest gap. `PassThruConnect` is two wire commands,
`cOpenChannel` (`0x06`) then `cSetPin` (`0x12`), and `PassThruDisconnect` is one,
`cCloseChannel` (`0x07`).

### Channels are addressed through `dst`, not through the `chan` field

`dst` is **not** simply the board node ID for channel commands:

| protocol | J2534 ID | `dst` | |
|---|---|---|---|
| CAN | 5 | `0x0501` | 1281 |
| ISO15765 | 6 | `0x0601` | 1537 |

so `dst = (protocol << 8) | board`, with board 1 as before. Device-level commands
(`cOpenDevice`, `cGetBoardInfo`, `cCloseDevice`) keep `dst = 0x0001`. Responses
swap `dst`/`src` exactly as documented. This was isolated by changing only the
protocol between two otherwise identical captures.

### `cOpenChannel` body is `u32 flags | u32 baud`

Isolated one variable at a time:

| capture | body | decode |
|---|---|---|
| CAN 500000 | `00000000 20a10700` | flags 0, baud `0x0007a120` = 500000 |
| CAN 250000 | `00000000 90d00300` | flags 0, baud `0x0003d090` = 250000 |
| CAN 500000 + `CAN_29BIT_ID` | `00010000 20a10700` | flags `0x00000100`, baud 500000 |

The flags word is the J2534 `ConnectFlags` value passed through verbatim
(`CAN_29BIT_ID` = `0x100`). The baud is a plain little-endian `u32` in bit/s.

### `cSetPin` carries the OBD-II pin routing

Body `01000000 06000000 0e000000` = `(1, 6, 14)` — identical for CAN and
ISO15765 and for both baud rates. Pins **6** and **14** are CAN High and CAN Low
on the OBD-II connector, so this is the pin pair the channel is bound to. The
leading `1` is unexplained; a pin-set index or an enable.

### Channel identifiers

The J2534 channel ID was `2` on every run, including after a disconnect and
reconnect, so it is not a monotonically increasing handle. The `chan` field at
body+8 stays `0` for all channel-management commands; it becomes `1` for data
commands (below). Channel *routing* is in `dst`; `chan` distinguishes streams
within a channel.

## D/E — Filters, receive and ISO15765

Captures: `…-d1-wildcard-pass` (32 live messages), `…-d3-flow-control-filter`,
`…-e1-obd-mode01-pid00`, `…-e2-obd-mode09-pid02-vin`.

### Two more opcodes identified

`PROTOCOL.md:232-265` lists `0x0d`–`0x10` only as unnamed "table ops". Two are now
pinned down by their use around `PassThruStartMsgFilter` / `PassThruStopMsgFilter`:

- **`0x000d` — add filter.** Body `02000000 40000000 000007e8 00000007e0 00`:
  a type word, TxFlags `0x40` (`ISO15765_FRAME_PAD`), the pattern CAN ID `0x7E8`,
  the flow-control CAN ID `0x7E0`, and a trailing byte. The **response carries the
  filter handle**: `0c492000` = `0x0020490c`.
- **`0x000e` — remove filter.** Body `02000000 0c492000` — the same type word and
  the handle returned by `0x000d`.

So the J2534 filter ID (`3` at the API) is a DLL-side index, not the wire handle;
the DLL maps between them. That answers the filter-ID mapping gap.

### Data path

- **`cOutboundData` (`0x08`)**, `chan=1`. Body
  `40000000 e8030000 06000000 000007df 0902` =
  `u32 TxFlags | u32 timeout_ms | u32 length | data`, where the data is the
  J2534 `PASSTHRU_MSG` payload — a 4-byte big-endian CAN ID followed by the
  service bytes. Here `0x40` = `ISO15765_FRAME_PAD`, 1000 ms, 6 bytes,
  request `09 02` to `0x7DF`. The response status is **256** (`0x100`), which is
  not an error — it accompanies a successfully queued transmit.
- **`cInboundData` (`0x09`)**. Body
  `00000000 <u32 timestamp> <u16 size> <u16 extra> <CAN ID> <payload>`. The
  timestamp is in the same device microsecond domain as the command responses.
- **`cIndication` (`0x0a`)** is unsolicited, device to host. Two codes observed:
  - `0x0106` — matches `iMsgTxDone` in `analysis/ENUMS.md`, carrying just a
    timestamp. Confirms that enum entry against hardware.
  - `0x010e` — reports a **transmitted ISO-TP flow-control frame**: timestamp,
    TxFlags `0x40`, CAN ID `0x7E0`, then `30 00 00 00` (FC, block size 0,
    STmin 0).

### Who does ISO-TP

The multi-frame VIN response is visible on the wire as **three separate
`cInboundData` frames with the ISO-TP PCI bytes intact** — `10 14` (first frame,
20 bytes), `21`, `22` (consecutive frames) — while the J2534 layer returned a
single reassembled 24-byte message plus a `RxStatus=2` start-of-message
indication. So **reassembly happens above the wire, in `monpj432.dll`**.

Flow control is the other way round: the `0x010e` indication reports the FC frame
as *already transmitted*, carrying the device's own timestamp, and no matching
`cOutboundData` is ever sent by the host. So **the adapter generates flow control
itself**. That split — firmware does flow control, the DLL does reassembly — is
the single most important thing to get right in a portable implementation.

## F — Ioctl

`READ_VBATT` returned **14100 mV** and **14000 mV** on consecutive calls with the
engine running, consistent with an alternator on charge. The Linux bench result of
`0` from `cGetValue` selector 3 was a no-vehicle reading, not a broken selector.

## Open after this round

`cGetValue` selector `0x2f`; the leading `1` of `cSetPin`; the three-transfer
`0xdb` preamble; the meaning of the `0x000d` type word; `cInboundData` body+0;
and `0x000f`/`0x0010`, still unnamed. Channel exhaustion (C3), concurrent
channels (C4), filter-table limits (D2) and sustained throughput (D4) were not
run.

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
