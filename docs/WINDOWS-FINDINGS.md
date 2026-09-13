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

### Filters use the table operations, and table selector 2 is the filter table

`PROTOCOL.md` already names `0x0d cTableAddEntry` and `0x0e cTableRemoveEntry`
(line 247-248) and already states that table operations carry a **table selector
at body+12**, warning that `0x0d` "is not exclusively filters" (line 211). Both
statements hold up, and the captures pin down the filter case:

- **`0x000d cTableAddEntry`.** Body `02000000 40000000 000007e8 00000007e0 00`:
  table selector **2**, then TxFlags `0x40` (`ISO15765_FRAME_PAD`), the pattern
  CAN ID `0x7E8`, the flow-control CAN ID `0x7E0`, and a trailing byte. The
  **response carries the table entry handle**: `0c492000` = `0x0020490c`.
- **`0x000e cTableRemoveEntry`.** Body `02000000 0c492000` — the same table
  selector and the handle returned by the add.

So table selector 2 is the message-filter table, and the J2534 filter ID (`3` at
the API) is a DLL-side index rather than the wire handle; the DLL maps between
them. That answers the filter-ID mapping gap. Selectors other than 2, and
`0x0f`/`0x10`, were never exercised by these captures.

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

## D4 — Sustained load

Capture: `20260913T164517-d4-sustained-load`. Five minutes of continuous receive on
a wildcard pass filter, CAN at 500 kbit/s, engine running.

```
stats_total=736512 rounds=2877 empty_rounds=0 requested_seconds=300
stats_device_span_us=299999000 rate_msg_per_s=2455.0
stats_unique_can_ids=69
stats_max_gap_us=6000 gaps_over_10ms=0
stats_rxstatus 0x00000000 = 736512
```

**Nothing was dropped and no back-pressure was ever signalled.** Four independent
checks agree:

- `RxStatus` was `0x00000000` for **every one of the 736 512 messages**. Not one
  overflow, buffer-full or protocol status bit appeared. So on plain CAN receive
  the status mask is simply always zero, and a portable implementation gets no
  loss signal from this field because the vendor never sets one here.
- `empty_rounds=0` — every `PassThruReadMsgs` returned a full batch.
- The largest gap between consecutive message timestamps was **6 ms**, with zero
  gaps over 10 ms. Dropped traffic would show as widening gaps.
- The device microsecond span was **299.999 s** across a 300 s window, so the last
  message was timestamped at the end of the run. A growing backlog inside the DLL
  would have left the timestamps lagging wall-clock; they did not.

The run reproduced **exactly** — two independent five-minute runs both returned
736 512 messages, because every round returned precisely the 256 requested
(2877 x 256). That is the J2534 contract working as specified rather than a
coincidence: `PassThruReadMsgs` returns when the requested count is reached or the
timeout expires, and at 2455 msg/s a batch of 256 takes 104 ms, which is exactly
the observed 300 s / 2877 rounds. The adapter and DLL sustained the full bus rate
in real time.

2455 msg/s across 69 distinct CAN IDs is roughly 60 % utilisation of a 500 kbit/s
bus, so this exercises the path properly without being a synthetic worst case.

This does not settle the `cdc_acm` throttling question, which is about a different
transport on Linux; it does establish that neither the adapter nor the vendor
stack is the bottleneck at this rate.

### On the artefacts

`wire.pcap` was **88 632 691 bytes** for this run and is not committed.
`wire-sample.pcap` is the first 2 MiB cut on a packet boundary (32 720 packets),
which is enough to show `cInboundData` arriving at rate. The findings above come
from the API statistics, not from the pcap, and the full capture is reproducible
by re-running the step script.

The first attempt at this test logged every message and produced a **69 MB
`api.log`** of 736 512 lines, which is unusable as evidence and unacceptable in a
repository. The `readstats` step exists for that reason: it prints eight sample
messages and then aggregates. The questions D4 asks — throughput, loss, which
status bits appear — are answered better by a histogram than by the raw stream.

## F — Ioctl

`READ_VBATT` returned **14100 mV** and **14000 mV** on consecutive calls with the
engine running, consistent with an alternator on charge. The Linux bench result of
`0` from `cGetValue` selector 3 was a no-vehicle reading, not a broken selector.

## B2 — PassThruReadVersion

Capture: `20260913T162339-b2-version`. The API returns

```
firmware = MongoosePro JLR FW:1.1.16.0 BL:1.1.8.0 SN: AOLHE0000003666A
driver   = MongoosePro JLR J2534 Library v1.1.16.0
api      = 04.04
```

and that string is assembled **DLL-side** from three wire commands:

| command | request | response |
|---|---|---|
| `cGetValue` selector `0x2b` | `2b000000` | `00100101` = `0x01011000` = 1.1.16.0 |
| `cGetValue` selector `0x2a` | `2a000000` | `00080101` = `0x01010800` = 1.1.8.0 |
| `cGetString` selector `0` | `00000000` | `AOLHE0000003666A\0` |

All three match the Linux bench results in `docs/VALIDATION.md` exactly, now
confirmed from the vendor side. The `driver` and `api` strings never touch the
wire — they are DLL constants.

## B3/B4 — Error paths and exclusivity

| stimulus | J2534 result | vendor text | wire traffic |
|---|---|---|---|
| `PassThruClose` twice | 26 `ERR_INVALID_DEVICE_ID` | `DeviceID 1 is invalid` | none |
| `PassThruReadVersion(9999)` | 26 `ERR_INVALID_DEVICE_ID` | `DeviceID 9999 is invalid` | **none** |
| `PassThruReadMsgs(9999)` | 2 `ERR_INVALID_CHANNEL_ID` | `ChannelID 9999 is invalid` | **none** |
| `PassThruOpen` twice, one process | 14 `ERR_DEVICE_IN_USE` | `'MongoosePro JLR #003666 (in use)' is valid but in-use` | — |
| `PassThruOpen` from a second process | 14 `ERR_DEVICE_IN_USE` | same | — |

Two things follow.

- **Handle validation is entirely DLL-side.** The invalid-device and
  invalid-channel cases produced *no pcap at all* — not an error round trip, no
  traffic whatsoever. A portable implementation must reject bad handles locally
  rather than asking the device.
- **Exclusivity is real and cross-process**, and the vendor reports it as
  `ERR_DEVICE_IN_USE` naming the serial. This is the behaviour `src/tty.cpp`
  approximates with `flock` + `TIOCEXCL`, so that approach matches vendor
  semantics rather than merely being defensible.

Device handles **increment and are not reused**: three open/close cycles in one
process returned device 1, 2, then 3 (`20260913T162358-b1-open-close-x3`). J2534
channel IDs behaved differently, staying at 2 across connect/disconnect.

## B7 — Opcode census

Across all 15 captures, **every opcode seen is already named** in the table at
`PROTOCOL.md:232-265` — including `0x000d`/`0x000e`, whose use for filters is
pinned down above. Nothing unknown appeared:

```
cOpenDevice 32   cCloseDevice 32   cOpenChannel 16   cCloseChannel 16
cOutboundData 4  cInboundData 9873 cIndication 3     cGetValue 40
0x000d 8         0x000e 8          cSetPin 16        cGetString 2
cEchoPacket 32   cJumpToFirmware 32 cGetBoardInfo 64 cCheckCRN 32
```

`dst`/`src` took exactly three values — `0x0001` (device level, 266 frames),
`0x0501` (CAN, 9897) and `0x0601` (ISO15765, 47) — with no exceptions, which is
strong support for the `(protocol << 8) | board` rule rather than a coincidence
of two samples.

**No destructive opcode appeared in any capture.** `cReflashBoard` (`0x10a`),
`cWriteSerialNumber` (`0x10b`), `cUnprotectBootloader` (`0x10c`) and
`cUpdateBTModule` (`0x112`) have a combined count of zero, so ordinary vendor
operation never approaches them. That was one of the plan's stop conditions and
it is worth restating: nothing observed here risks the adapter.

## A3 — Vendor debug log: negative result

`DebugEnable` was set on the vendor's PassThru key as both `REG_DWORD 1` and
`REG_SZ "1"`, and a full open/version/close cycle was run under each.
`C:\DrewTech` was **never created** and no log file appeared anywhere.

So the `DebugEnable` string is not, by itself, the gate for the logging
facility whose format string and diagnostics are visible in `monpj432.dll`. The
value may be read from a different key, combined with another condition, or be
dead code in this build. The registry value has been removed and the key restored
to its original state; `analysis/captures/windows/passthru-key-backup.reg` holds
the pre-test export. Pursuing this further means disassembling the `DebugEnable`
reference rather than guessing at more value types.

This route is closed unless that disassembly says otherwise. It cost little and
the captures do not depend on it.

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
