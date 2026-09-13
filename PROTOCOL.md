# MongoosePro JLR — protocol research

Updated 2026-09-13. Static evidence from the supplied DLL and kernel driver, cross-checked
with x86/x64 assembly. **Linux discovery, device open/close and value queries now succeed.**
See `docs/VALIDATION.md` and `analysis/captures/linux-*.trace` for the new hardware
evidence. The static-research descriptions below describe their original evidence
boundary; they do not override the newer validation record. No vehicle was attached.
“Observed” below means code behavior; it does not claim hardware validation.
Earlier speculative framing is archived in `analysis/history/PROTOCOL-before-deep-ghidra.md`.

## 1. Transport and the previously missing initialization

Device VID:PID `18e1:0104`. Linux exposes `/dev/ttyACM0`; prior captures showed writes reach
bulk OUT 0x01 unchanged. Bulk IN is 0x82; interrupt IN is 0x83.

The Windows driver does more than expose bulk endpoints: **file creation sends a vendor
USB control transfer**. `kernel_labeled/000179b4.c` registers `00017834` as file-create and
`000178fc` as file-cleanup. Both call `000176e0`:

| Setup field | Create | Cleanup |
|---|---|---|
| bmRequestType | 0x40 | 0x40 |
| bRequest | 0xdb | 0xdb |
| wValue | 1 | 0 |
| wIndex | 0 | 0 |
| wLength | 0 | 0 |

Setup bytes: `40 db 01 00 00 00 00 00` on create; value becomes zero on cleanup.
Assembly at `17740`–`17782` zeroes the setup structure and passes it with no data buffer to
WdfUsbTargetDeviceSendControlTransferSynchronously. See `analysis/disassembly/kernel_vendor_control.asm`.
The create callback completes successfully without checking this helper's transfer result.

This is a concrete difference from opening a Linux tty. It was previously suspected to be a
missing prerequisite for the earlier silent probes. **It is not** — section 7a records a
hardware test in which the device answered normally over plain `cdc_acm` after a USB reset
with no control transfer of any kind. The firmware meaning of the request remains unverified;
the most likely reading is file-handle open/close bookkeeping.

Bulk path: `EvtIoWrite` at `00011df4` retrieves the original input memory, formats that same
memory for a USB pipe write, and sends it. `EvtIoRead` at `00011870` similarly forwards output
memory to a USB pipe read. Neither inspected path serializes, escapes, or checksums the body.
Each rejects request lengths above 65536. The USB configuration code identifies bulk IN/OUT
and interrupt pipes; the interrupt reader stores one received byte at device-context+0x28.
Its meaning has not been established (`000123a8`, `00012444`, `00018560`).

Kernel API names were resolved using table base 0x141c0, eight-byte entries, count 396, and
Microsoft's [KMDF function indexes](https://github.com/microsoft/Windows-Driver-Frameworks/blob/main/src/publicinc/wdf/kmdf/1.15/wdffuncenum.h).
The binary binds KMDF 1.9; the available 1.15 header's relevant indexes are corroborated by
call shapes and callback registrations. File callback ordering is documented in Microsoft's
[WDF_FILEOBJECT_CONFIG](https://github.com/microsoft/Windows-Driver-Frameworks/blob/main/src/publicinc/wdf/kmdf/1.15/wdfdevice.h).

## 2. Framing and common body

All offsets in body tables exclude the four-byte transport prefix. Multi-byte stores are little-endian.

| Wire offset | Size | Value |
|---|---|---|
| 0 | 2 | Body length N |
| 2 | 2 | N XOR 0x51e6 |
| 4 | N | Body |

`1006e180` copies the body unchanged and sends N+4 bytes through `1006a420` to WriteFile
at `1006a473`. Initialization at `1006ae00` sets the XOR constant; `1006dd43` establishes
its subobject offset. Message batching uses the same prefix. This is a length check, not a
checksum over the body. No body checksum is added in the traced control or message senders.

Receive worker `1006e480` reads up to 8192 bytes and feeds `1006b1c0`, which accumulates bytes
in a ring buffer. It accepts 1 <= N <= 0x1800, checks the XOR, waits for all N+4 bytes, dispatches
the body, and consumes the frame. Invalid headers advance by one byte to seek synchronization.
It therefore handles fragmented and concatenated frames; USB packet boundaries are not frame boundaries.

| Body offset | Size | Observed field |
|---|---|---|
| 0 | 2 | Routing word A; device requests use 1, channel requests use a channel word |
| 2 | 2 | Routing word B; traced requests normally use 0 |
| 4 | 2 | Opcode |
| 6 | 2 | Sequence; control sender callers widen a nonzero byte counter |
| 8 | 2 | Zero in simple requests; used by some other message types |
| 10 | 2 | Not explicitly initialized by inspected simple-request builders |
| 12 onward | variable | Command-specific data |

Routing A/B **are** destination/source identifiers: confirmed in section 7a from `.rdata`
constants, the `board-PC` / `board-1`…`board-8` node names, and a hardware round trip in which
the response swaps the two words. PC is node 0; boards are 1–8.
Do not silently assume bytes 10–11 are zero: no explicit store initializes them in the
inspected simple builders, in either the `cCheckCRN` or `cOutboundData` builder.
Section 7b settles what that means on the wire — the vendor really does send whatever
happened to be in caller memory, and the firmware echoes the field back verbatim without
acting on it, so sending zero is safe but reading it as meaningful is not.

### Response queue and matching — corrected

`10043610` reads opcode at body+4 and routes ordinary responses unchanged to `1006c5e0`.
After correcting register signatures, `1006c660` clearly inserts:

`LE32(N) | N body bytes | LE32(N)`

`1006c6e0` returns the body and consumes N+8 internal bytes. The trailing word is a duplicate
length, **not a wire checksum**. `1006b480` computes a new cursor into caller-provided storage;
it does not commit the FIFO write cursor. Earlier helper descriptions were incorrect.

Both response matchers compare **response body+0 with request body+2**, and compare sequence
at body+6. Neither inspected matcher compares opcode with request_opcode|0x8000. Dispatch
already restricts which message types enter the queue. `1006d9b0` requires at least 20 returned
bytes and constructs a deadline about ten seconds ahead. The simpler `1006d810` requires 12.

For general responses inspected through the open and configuration callers:
- Body+12: 32-bit status.
- Body+16: **device microsecond counter**, reset to zero by `cOpenDevice` (section 7a).
- Body+20: returned command data where present, or NUL-terminated ASCII error text on failure.

Sources: `deep_transport/`, `receive/`, `refined_fifo/`, `open_transport/10043610.c`, and
`open_sequence/1006d9b0.c`. `RefineFifoSignatures.java` reproduces the in-memory corrections
with `-readOnly`; the original Ghidra database remains unchanged.

## 3. Opening sequence and simple requests

New-context hardware initialization: enumerate device interface -> exclusive overlapped
CreateFileW -> reset events -> mark running -> pthread_create(receive worker). The function
pointer at 0x100a3170 resolves to `pthread_create`, with entry point `1006e480`.
CreateFile triggers the kernel vendor request described above.

The observed normal-open path in `100500c0` then sends:

| Step | Function | Opcode | Body bytes | Accepted response status |
|---|---|---|---|---|
| Start firmware | 1003e820 | 0x0103 | 12 | 0 or 7; meaning of 7 unresolved |
| Open device | 1003ea00 | 0x0003 | 12 | 0 |
| Read board info | 1003f9c0 | 0x0109 | 12 | 0 |

Open status 0x020a is explicitly reported as missing voltage on the vehicle connector.
This shows vehicle power can matter; it does not explain the earlier lack of all responses.

A separate discovery path (`1004a3f0`) initializes transport, sends Echo (`1003f030`, 0x0100),
then GetBoardInfo (`1003f160`). It conditionally starts firmware and reads board info again.
Thus cOpenDevice is not necessary for every information query in the vendor implementation.
Echo uses the simpler response matcher and logs a possible dropped first response if it times out.

Static cOpenDevice buffer:

`0c 00 ea 51 01 00 00 00 03 00 SS 00 00 00 ?? ??`

SS is the nonzero sequence byte; ?? means uninitialized/unknown bytes, not wildcards to transmit.
Echo changes the opcode bytes to `00 01`; GetBoardInfo uses `09 01`. These are construction
examples, not hardware-validated probe instructions.

Sources: `open_sequence/`, `open_transport/`, `usb_open/`, `message_decode/1004a3f0.c`,
`senders/1003f030.c`, `senders/1003f160.c`, and saved disassembly.

### Channel and configuration requests

| Function | Opcode | Body size | Fields after common header |
|---|---|---|---|
| 1000b3c0 | 0x0006 open channel | 20 | +12 u32 caller argument; +16 u32 caller argument |
| 1000b690 | 0x0007 close channel | 12 | None |
| 1000dd00 | 0x000c get value | 16 | +12 u32 selector |
| 1000de10 | 0x000b set value | 20 | +12 u32 selector; +16 u32 value |
| 10040240 | 0x0013 get string | 16 | +12 u32 selector; returned string read at response+24 |
| 1000daf0 | 0x0011 ioctl | 16 | +12 u32 2: clear TX |
| 1000dbf0 | 0x0011 ioctl | 16 | +12 u32 3: clear RX |
| 1002a700 | 0x0011 ioctl | 17 | +12 u32 0; +16 initialization address byte (5-baud init) |

Open-channel argument semantics and the source of channel IDs still need tracing through
protocol-specific constructors; do not substitute SAE protocol IDs or assume flag/baud order.
`1000b320` additionally calls the pin-control sender after channel open. Channel creation is
therefore more than issuing opcode 6 alone.

### Raw outbound and inbound messages

`1006b090` constructs cOutboundData (0x0008) from the vendor's PASSTHRU_MSG-shaped input:

| Body offset | Size | Outbound value |
|---|---|---|
| 0 | 2 | Selected channel word |
| 2 | 2 | 0 |
| 4 | 2 | 8 |
| 6 | 2 | Caller argument (sequence field) |
| 8 | 2 | Caller argument |
| 10 | 2 | No explicit store in this builder |
| 12 | 4 | Input message+8 (TxFlags) |
| 16 | 4 | Caller argument; meaning unresolved |
| 20 | 2 | Input message+16 (DataSize, truncated to a word) |
| 22 | 2 | Input message+20 (ExtraDataIndex, low word) |
| 24 | DataSize | Input message data at +24 |

Body length is DataSize+24; total transfer frame is DataSize+28. `1006b120` is a variant
selecting between channel words using the input protocol field. `1006e2e0`/`1006e3b0`
concatenate frames into the transport buffer, allowing one WriteFile to contain multiple frames.

For ordinary inbound data (opcode 9), dispatcher `10043610` forwards body+12 to the channel
callback. `1000e4c0` -> `1000b790` decodes:

| Body offset | Size | Inbound value |
|---|---|---|
| 12 | 4 | Receive status, masked by a channel-specific virtual method |
| 16 | 4 | Timestamp copied into the application message |
| 20 | 2 | Not consumed by this decoder |
| 22 | 2 | DataSize |
| 24 | DataSize | Message data |

The decoder sets TxFlags=0 and ExtraDataIndex=DataSize, caps the copied data at 0x1020,
and queues an application message. Timestamp units and protocol-specific status masks remain
unverified. The opcode 10 indication path has separate handling.

**ISO15765 is not wholly delegated to firmware.** The DLL contains host-side receive
reassembly: `10021790` dispatches single/first/consecutive frames; `10021280` checks the
four-bit sequence and copies fragments until the announced length is met. Wrong sequence
resets receive state. `10021070` contains first-frame/flow-control handling; standard and
extended-address paths differ. A raw CAN client and a complete J2534 ISO15765 implementation
therefore have different scope. See `isotp/` and `message_payload/`.

### Filters and periodic messages

Table operations carry a table selector at body+12; opcode 0x0d is not exclusively filters.
Observed selectors: 0/1 pass/block filters, 2 flow filter, 3 functional addresses, 4 periodic
messages, 5 repeat messages. These are wire table selectors, not J2534 enum values.

Normal pass/block builder `1000e600` sends N=20+mask_size+pattern_size:
+12 u32 table selector 0/1; +16 u16 masked TxFlags (0x100); +18 u8 type 1/2;
+19 u8 pattern size; +20 mask bytes then pattern bytes. The size comes from an 8-bit store,
so upstream constraints must be checked before implementing arbitrary-size inputs.

Periodic builder `1000d220`: +12 u32 4; +16 u32 caller interval argument; +20 u32 message
TxFlags; +24 u8 DataSize; +25 message data. N=25+DataSize. The request sequence is also saved
for asynchronous response bookkeeping. Flow filters and repeat builders were extracted but
are not yet fully specified; see `senders/10021970.c` and `senders/1000d5d0.c`.

## 4. Command/response opcode table

Source: `analysis/decompiled/strref_FUN_1003b080_1003b080.c` (a pure opcode→name lookup function,
fully decompiled with no ambiguity). Named command-specific response opcodes below
use request opcode `| 0x8000`. General responses also exist; matching uses routing
and sequence after dispatcher filtering, not this arithmetic rule.

| Opcode | Name | Opcode | Name |
|---|---|---|---|
| 0x00 | cPrintDebugText | 0x109 | cGetBoardInfo |
| 0x01 | cRespGeneral | 0x10a | cReflashBoard |
| 0x02 | cRespString | 0x10b | cWriteSerialNumber |
| 0x03 | cOpenDevice | 0x10c | cUnprotectBootloader |
| 0x04 | cGetDeviceConfiguration | 0x10d | cBrownoutIndication |
| 0x05 | cCloseDevice | 0x111 | cCheckCRN |
| 0x06 | cOpenChannel | 0x112 | cUpdateBTModule |
| 0x07 | cCloseChannel | | |
| 0x08 | cOutboundData | **Responses (request \| 0x8000):** | |
| 0x09 | cInboundData | 0x8003 | cOpenDeviceResp |
| 0x0a | cIndication | 0x8005 | cCloseDeviceResp |
| 0x0b | cSetValue | 0x8006 | cOpenChannelResp |
| 0x0c | cGetValue | 0x8007 | cCloseChannelResp |
| 0x0d | cTableAddEntry | 0x8008 | cOutboundDataResp |
| 0x0e | cTableRemoveEntry | 0x800b | cSetValueResp |
| 0x0f | cTableModifyEntry | 0x800c | cGetValueResp |
| 0x10 | cTableClear | 0x800d | cTableAddEntryResp |
| 0x11 | cIoctl | 0x800e | cTableRemoveEntryResp |
| 0x12 | cSetPin | 0x8010 | cTableClearResp |
| 0x13 | cGetString | 0x8011 | cIoctlResp |
| 0x14 | cSetData | 0x8012 | cSetPinResp |
| 0x15 | cGetData | 0x8013 | cGetStringResp |
| 0x100 | cEchoPacket | 0x8014 | cSetDataResp |
| 0x101 | cGetBoardStatus | 0x8015 | cGetDataResp |
| 0x102 | cResetBoard | 0x8102 | cResetBoardResp |
| 0x103 | cJumpToFirmware | 0x8103 | cJumpToFirmwareResp |
| 0x104 | cSetBoardID | 0x8109 | cGetBoardInfoResp |
| 0x105 | cSetBoardLed | 0x810a | cReflashBoardResp |
| 0x106 | cSyncClock | 0x810b | cWriteSerialNumberResp |
| 0x107 | cGetStats | 0x810c | cUnprotectBootloaderResp |
| 0x108 | cBoardSleep | 0x8111 | cCheckCRNResp |
| | | 0x8112 | cUpdateBTModuleResp |

**Observed on hardware but absent from the lookup table:** `0x8100`, the `| 0x8000`
response to `0x100 cEchoPacket`. `analysis/captures/linux-discovery-20260912T2030.trace`
contains `...000081010000...` — opcode `0x8100`, sequence 1, carrying a **12-byte** body
rather than the 20-byte general response. `src/session.cpp` therefore accepts `0x8100`
and lowers its minimum accepted body length for opcode `0x100`. The table above is a
faithful transcription of the vendor name-lookup function, which simply has no name
string at `0x8100`/`0x8101`; that gap is an omission in the vendor table, not evidence
the opcode is invalid. Do not "correct" the decoder to match the table.

### Mapping to the J2534 API

| J2534 export | Wire opcode |
|---|---|
| `PassThruOpen` | `cOpenDevice` |
| `PassThruClose` | `cCloseDevice` |
| `PassThruConnect` | `cOpenChannel` |
| `PassThruDisconnect` | `cCloseChannel` |
| `PassThruWriteMsgs` | `cOutboundData` |
| `PassThruReadMsgs` | Queued `cInboundData`; indications have separate handling |
| `PassThruStartMsgFilter` | `cTableAddEntry` |
| `PassThruStopMsgFilter` | `cTableRemoveEntry` |
| `PassThruIoctl` | Dispatches to get/set value, table operations, or `cIoctl`, depending on operation |
| `PassThruSetProgrammingVoltage` | `cSetPin` |

## 5. J2534 ConnectFlags (standard, from `strref_FUN_100384d0_100384d0.c`)

| Value | Flag |
|---|---|
| 0x100 | CAN_29BIT_ID |
| 0x200 | CHECKSUM_DISABLED |
| 0x800 | CAN_ID_BOTH |
| 0x1000 | ISO9141_K_LINE_ONLY |
| 0x10000000 | DT_SNIFF_MODE (vendor extension) |

These are the standard SAE J2534-1 connect flags plus one Drew Tech vendor extension
(`DT_SNIFF_MODE`) — nothing proprietary to reverse here, included for completeness.

## 6. J2534 SConfig parameter IDs (standard + vendor, from `strref_FUN_10038b70_10038b70.c`)

Full standard J2534 SConfig list (DATA_RATE, LOOPBACK, P1_MAX/P2_MAX/etc timing parameters,
ISO15765_BS/STMIN, etc.) plus Drew Tech vendor extensions starting at `0x10000000`:

| Value | Name |
|---|---|
| 0x10000001 | DT_ISO15765_PAD_BYTE |
| 0x10000005 | DT_J1939_CTS_BS |
| 0x10000006 | DT_RETRY_MAX |
| 0x10000007 | DT_HALF_DUPLEX |
| 0x10000008 | DT_ISO_INIT_BAUD |

Also `0x10008 DT_PULLUP_VALUE`. See the decompiled file for the complete standard-range table.

## 7. Firmware-assisted K-line initialization

ISO9141/ISO14230 5-baud slow-init handshakes (`analysis/decompiled/strref_FUN_1002a700_1002a700.c`,
`analysis/decompiled/strref_FUN_1001b800_1001b800.c`) are implemented as a single high-level
`cIoctl` (0x11) call to the adapter — the time-critical K-line bit-banging (send 0x33 at 5 baud,
receive sync byte 0x55, receive two key bytes, send inverted key byte, receive inverted address)
is delegated to the adapter firmware by the inspected DLL path. The host command layout and
response handling still need implementation and hardware validation. This finding concerns
K-line initialization; it does not imply ISO15765 reassembly is wholly firmware-managed.

## 7a. Independent hardware pass — corrections and confirmations (2026-09-13)

A second reverse-engineering pass was run from the vendor binaries alone (Ghidra 12.1.2
headless, machine-extracted switch tables) and then checked against the adapter over
**plain `cdc_acm`**. No vehicle attached. Probe scripts: `analysis/probes/` (see its README; the shared protocol lives in
`mongoose_wire.py`, and `Device.call()` refuses flash/identity writes unless explicitly
overridden).
Only read-only opcodes were issued; reflash, bootloader, serial-write, board-ID,
sleep, reset and jump-to-firmware were deliberately excluded.

### Vendor control request 0xdb is **not** a prerequisite — corrected

Sections 1 and 8 treat the file-create request `40 db 01 00 00 00 00 00` as a plausible
missing initialization step. It is not required for the command/response layer.

Test: `USBDEVFS_RESET` was issued on the device (forcing re-enumeration and clearing any
firmware state a previous libusb session may have latched), `cdc_acm` re-bound both
interfaces, and commands were then sent to `/dev/ttyACM*` with **no control transfer of
any kind**. Echo, GetBoardInfo and GetString all answered correctly on the first attempt
with zero resync slides. Reproduce with `analysis/probes/reset_test.py`.

Consequence: a Linux client does not need libusb, interface detachment, udev rules or
root — an ordinary `/dev/ttyACM*` reader/writer with `dialout` membership is sufficient.
The request is most likely the Windows driver telling the firmware that a file handle was
opened (`wValue` 1) or closed (`wValue` 0). This finding covers the message layer that was
exercised; it does not speak to bus traffic or programming voltage.

### Routing words A/B are destination/source — confirmed, no longer an inference

Three independent lines of evidence:

* The `cCheckCRN` builder loads body+0 and body+2 from `.rdata` constants `1` and `0`
  (`0x100806c8`, `0x100806c4`); the `cOutboundData` builder loads body+0 from a channel word.
* The DLL contains the node names `board-PC` and `board-1` … `board-8`, matching PC = 0,
  boards = 1…8, alongside `cSetBoardID`.
* On hardware every response swaps them: request `dst=1 src=0` → response `dst=0 src=1`.

### Response body+16 is a device microsecond counter — resolves an open item

Sections 2 and 8 list `response+16` and "timestamp units" as unresolved. Across probes
separated by a fixed 0.8 s read window the field advanced
1,602,688 → 2,404,388 → 4,007,488, i.e. ≈801,700 per 0.8 s: **microseconds**.

`cOpenDevice` returns this field as `0`, so the command **resets the device clock**; a
cold adapter reports its uptime instead (≈4.02×10⁹ µs ≈ 67 min on an idle unit). This is
the origin used for J2534 message timestamps.

### Firmware returns self-documenting errors

Failed responses carry NUL-terminated ASCII beginning at body+20. Observed:

| Status | Text |
|---|---|
| 0x0007 | `ProcessCommand: Unknown/Unhandled Command` |
| 0x0203 | `cGetValue: Invalid message length.` |
| 0x0003 | `cGetValue: Unsupported or Invalid Resource` |
| 0x0007 | `cGetData: Unsupported or Invalid data type` |

This generalizes the previously unresolved "status 7": it is a generic failure whose
explanatory text is in the body, consistent with the `Board already in firmware` text
already recorded for StartFirmware in `docs/VALIDATION.md`.

### Response opcodes absent from the name table

The table in section 4 is incomplete for responses to the 0x1xx range. Confirmed on the
wire, in addition to the already-noted `0x8100`: **`0x8101` cGetBoardStatusResp** and
**`0x8107` cGetStatsResp**. All three were predicted by `request | 0x8000` before being
observed, so that rule holds for generating response opcodes even though the DLL's
*matchers* do not use it (they filter by dispatch, then routing and sequence).

### Hardware-observed command details

* `cGetValue` (0x0c): body must be **exactly** a 4-byte selector — 2 bytes or 8 bytes both
  return `Invalid message length`. Selector 1 succeeded; the rest returned
  `Unsupported or Invalid Resource` with no channel open.
* `cGetString` (0x13): returned `AOLHE0000003666A`, byte-identical to the USB `iSerial`.
* `cGetBoardStatus` (0x101) returns a 28-byte body that is exactly the **first 28 bytes**
  of the 168-byte `cGetBoardInfo` (0x109) body — a shared board header:

      +0  u32 status 0        +12 u32 45236 (0xB0B4)   +20 00 08 01 01
      +4  u32 timestamp       +16 u32 21976 (0x55D8)   +24 00 10 01 01
      +8  05 01 01 01

  Bytes +20 and +24 are consistent with the firmware 1.1.16.0 and bootloader 1.1.8.0
  values already recorded in `docs/VALIDATION.md` (`00 10 01 01`, `00 08 01 01`),
  which identifies those two words. The `0xB0B4` / `0x55D8` fields remain unnamed.
* `cGetDeviceConfiguration` (0x04) is **not implemented** by this firmware — it returns
  status 7 `Unknown/Unhandled Command`.

### Board state machine — `cResetBoard` drops to the bootloader

Hardware sequence, adapter only, no vehicle (`analysis/probes/probe_boardstate.py`):

| Step | Result |
|---|---|
| `cGetBoardStatus` | type 5, status **1 FIRMWARE**, bootloader 1.1.8.0, firmware 1.1.16.0 |
| `cResetBoard` (0x102) | status 0, body text `FW RESET!!`, then USB re-enumeration |
| `cGetBoardStatus` | type 5, status **2 BOOTLOADER** |
| `cJumpToFirmware` (0x103) | status 0 |
| `cGetBoardStatus` | type 5, status **1 FIRMWARE** — fully recovered |

`cResetBoard` therefore restarts **into the bootloader**, which is exactly why the vendor's
open path (section 3) issues `cJumpToFirmware` as step 1 and tolerates its "already in
firmware" status. Both transitions are non-destructive and were exercised repeatedly.

The bootloader implements a **reduced command set**. `cEchoPacket`, `cGetBoardStatus`,
`cGetBoardInfo`, `cGetString` and `cCheckCRN` answer normally; `cGetStats` and `cOpenDevice`
return status 1 `eNotSupported` with the text `Invalid or Unhandled command type`. The
firmware instead returns status 7 `eFailed` / `ProcessCommand: Unknown/Unhandled Command`
for commands it does not implement, so the status code distinguishes the two modes.

Board header (body+8 of `cGetBoardStatus` / `cGetBoardInfo`), now resolved:

    +8  u8  board type      5
    +9  u8  board status    1 FIRMWARE / 2 BOOTLOADER / 3 SLEEP / 0 UNKNOWN
    +10 u8, +11 u8          0x01 0x01
    +12 u32 0x0000B0B4      unnamed
    +16 u32 0x000055D8      unnamed
    +20 u32 bootloader version   0x01010800 = 1.1.8.0
    +24 u32 firmware version     0x01011000 = 1.1.16.0

The two version words match the values `docs/VALIDATION.md` previously retrieved through
selectors 0x2a and 0x2b, which identifies them.

`cSetBoardLed` (0x105) and `cSyncClock` (0x106) return no response at all, consistent with
their absence from the response half of the opcode table — they are fire-and-forget.

### Firmware images are embedded in the DLL

`monpj432.dll` carries both images as `RT_RCDATA` resources (extract with
`analysis/extract_firmware.py`):

| Resource | Bytes | Header name | Version |
|---|---|---|---|
| 5005 | 112780 | `MongoosePro Jaguar Firmware` | 0x01011000 = 1.1.16.0 |
| 5006 | 19876 | `MongoosePro Jaguar Bootloader` | 0x01010800 = 1.1.8.0 |

Image header: `u32 header_size (0x6c) | u16 image_type (1 bootloader, 2 firmware) |
u16 board_type (5) | u32 version | char name[] (NUL padded)`, then a high-entropy payload
(compressed or encrypted; not decoded). The versions and board type match the attached
adapter exactly, so the shipped images are the ones it is running.

### Reflash protocol — static layout, not executed

`FUN_1003f480` (`reflash start`) builds `cReflashBoard` (0x10a):

    +0  u16 dst = 1            +12 u32 subcommand = 1 (start)
    +2  u16 src = 0            +16 u32 image/board selector (switch over caller arg, 1..8)
    +4  u16 opcode = 0x10a     +20 u32 caller argument (image size)
    +6  u16 sequence           +24 ... image bytes
    +8  u16 0

`FUN_1003f770` is the continue phase ("Waiting for reflash continue response"); `FUN_10040b40`
drives the loop and logs `reflash start` / `reflash loop done` / `reflash complete!`.
Failures use the 0x03xx status band (`eReflashWrongBoardType`, `eReflashInvalidImage`,
`eReflashNoResponse`, `eReflashInvalidState`, `eReflashInvalidParameter`).

**This layout is not settled.** The length handed to `frame_send` is `n + 0x1c`, while the
image `memcpy` lands at payload+24, leaving four bytes unaccounted for. That discrepancy must
be resolved before any write is attempted — an off-by-four in a flash-write frame is precisely
the error that corrupts an image. **No reflash, bootloader-unprotect, serial-number write or
Bluetooth-module update has been executed.**

### Enumerations

Sections 5–6 describe the SConfig and IOCTL lists in prose and point at decompiled files.
The complete machine-extracted tables are now in [analysis/ENUMS.md](analysis/ENUMS.md):
95 SConfig parameter IDs, 44 IOCTL IDs, 27 J2534 error codes, **27 protocol IDs**,
27 channel capability IDs, 75 device capability IDs and the **66 wire status/indication
codes** of `FUN_10037c10`. That last table names every status observed on the wire —
`0x0203 eInvalidMsgLength`, `0x0001 eNotSupported`, `0x0007 eFailed` — and identifies the
previously unexplained open status `0x020a` as **`eVbattLoss`**. Its `0x01xx` band is the
`cIndication` code space: the dispatcher's `body+12 == 0x106` test is **`iMsgTxDone`**. The protocol-ID table is the one
section 3 warns about substituting from the SAE spec; the adapter's own values are
`CAN = 5`, `ISO15765 = 6`, with pin-select variants `CAN_PS = 0x8004`,
`ISO15765_PS = 0x8005` — the pair relevant to the 2017 Volvo XC60.

## 7b. Windows reference captures — the channel layer (2026-09-13)

Captured from the vendor driver itself on Windows 11 against the real adapter and
a 2017 Volvo XC60 D5 AWD. Full write-up and per-claim capture citations are in
`docs/WINDOWS-FINDINGS.md`; raw material is under `analysis/captures/windows/`.
The installed `monpj432.dll` is byte-identical to `vendor/driver/monpj432.dll`,
so this section and the static analysis describe the same binary.

### Channel commands are addressed by `(protocol << 8) | board` — resolves an open item

`dst` is not always the board node. Device-level commands use `0x0001` as
documented, but channel commands are addressed to a per-protocol node:

| protocol | J2534 ID | `dst` |
|---|---|---|
| CAN | 5 | `0x0501` |
| ISO15765 | 6 | `0x0601` |

Isolated by changing only the protocol between two otherwise identical captures,
and consistent across every frame in the corpus: `dst`/`src` took exactly three
values, `0x0001`, `0x0501` and `0x0601`, with no exceptions. Responses swap
`dst`/`src` as usual. `src/codec.hpp` implements this as `channel_node()`.

### The two `cOpenChannel` arguments — resolves an open item

Body is `u32 ConnectFlags | u32 baud`, each confirmed by a single-variable change:
baud 500000 gives `0x0007a120` and 250000 gives `0x0003d090`, while
`CAN_29BIT_ID` puts `0x00000100` in the flags word. The flags are the J2534
`ConnectFlags` value passed through verbatim.

### Pin routing — resolves an open item

Every `PassThruConnect` is followed by `cSetPin` (`0x12`) with body
`01000000 06000000 0e000000` = `(1, 6, 14)`. Pins **6** and **14** are CAN High
and CAN Low on the OBD-II connector. Identical for CAN and ISO15765 and for both
baud rates. The leading `1` is unexplained.

### Body+10 is an echoed token, not a reserved field — resolves an open item

Section 2 warns against assuming bytes 10–11 are zero because no explicit store
initializes them. The captures settle what the firmware does with them: the
vendor sends unrelated values (`0x7719`, `0x008f`, `0xffff`, `0x0000` — two
different values for the same opcode within one run, consistent with
uninitialised caller memory) and the firmware **echoes the field back verbatim**
in the response without acting on it. Sending zero is therefore safe, and
`request()` in `src/codec.cpp` does so deliberately.

### Table selector 2 is the message-filter table

`cTableAddEntry` (`0x0d`) with selector 2 adds a filter and **returns the entry
handle in its response**; `cTableRemoveEntry` (`0x0e`) removes it by that handle.
The J2534 filter ID is a DLL-side index, not the wire handle. This resolves the
filter-ID mapping item. Other selectors and `0x0f`/`0x10` were not exercised.

### ISO15765 responsibility is split — matters for any portable implementation

- **The adapter generates flow control.** Indication `0x010e` reports an FC frame
  (`30 00 00 00` to `0x7E0`) as *already transmitted*, carrying the device's own
  timestamp, and the host never sends a matching `cOutboundData`.
- **The DLL reassembles.** A multi-frame response arrives as separate
  `cInboundData` frames with ISO-TP PCI bytes intact (`10 14`, `21`, `22`), while
  the J2534 layer returns one reassembled message.

Indication `0x0106` matches `iMsgTxDone` in `analysis/ENUMS.md`, confirming that
enum entry against hardware.

### Data commands

`cOutboundData` body is `u32 TxFlags | u32 timeout_ms | u32 length | data`, where
data is the J2534 payload — a four-byte big-endian CAN ID then the service bytes.
Its response status is `0x100`, which accompanies a successful queue, not an
error. `cInboundData` body is `u32 | u32 timestamp | u16 size | u16 extra |
CAN ID | payload`, the timestamp in the same device microsecond domain.

### Handle validation is DLL-side

`PassThruReadVersion` and `PassThruReadMsgs` with invalid identifiers produced
**no wire traffic at all** — not an error round trip. The DLL rejects bad handles
locally. Device exclusivity is enforced across processes, reported as
`ERR_DEVICE_IN_USE`, which is the behaviour `src/tty.cpp` approximates with
`flock` plus `TIOCEXCL`. Device handles and channel IDs both increment and are not
reused within a process. (An earlier revision said channel IDs were reused; that was a
mistake from comparing separate process runs, each of which numbers from scratch.
Within one process a connect/disconnect cycle returns 2, 3, 4.)

### Safety observation

No destructive opcode appeared anywhere in the corpus: `cReflashBoard` (`0x10a`),
`cWriteSerialNumber` (`0x10b`), `cUnprotectBootloader` (`0x10c`) and
`cUpdateBTModule` (`0x112`) have a combined count of zero across all captures, so
ordinary vendor operation never approaches them.

## 8. Remaining work and validation boundary

The discovery Echo/GetBoardInfo flow has since been reproduced on hardware without the
vendor-create control transfer (section 7a, `analysis/probes/`). The old sweep script does not
implement the corrected protocol; do not treat its candidate frames as valid.
Channel open and bus traffic against a vehicle have now been captured from the vendor
driver on Windows (section 7b), which closes most of what this list used to contain.

Resolved since this list was written, all in section 7b: channel addressing and the two
open-channel arguments, pin routing, body+10 semantics, and filter-ID mapping.
`response+16` and timestamp units were resolved in section 7a (microseconds, zeroed by
`cOpenDevice`), and vendor request `0xdb` in section 7a (not required for the message layer;
Windows sends it anyway).

Still open:
- **body+8 (`chan`)** is 0 for channel management and 1 for data commands. It stays
  unresolved, and now looks unresolvable on this hardware: the adapter permits only one
  CAN-family channel at a time (section 7b), so no arrangement of channels on this unit can
  produce further values to compare.
- **Per-protocol status masks** remain unmapped for transmit and for protocols other than
  CAN. On plain CAN receive the mask is now characterised and it is trivial: `RxStatus` was
  `0x00000000` for all 736512 messages of a five-minute sustained run (section 7b), so the
  field carries no loss signal there. `cOutboundData` returning status `0x100` on success is
  the only transmit status value characterised.
- **ISO15765 timing.** The responsibility split is known (firmware does flow control, the DLL
  reassembles) but no timing parameter — STmin, block size, N_Bs — has been varied or measured.
- **Channel concurrency is settled and restrictive** (section 7b): one CAN-family channel
  at a time, enforced DLL-side with no wire traffic. A second CAN connect is refused with
  `There's already a 5:CAN channel open`, and an ISO15765 connect while CAN is open with
  `All 6:ISO15765 hardware is busy` - the two protocols share one CAN controller.
- **The filter table limit was not found.** Forty pass filters were accepted on one channel;
  the limit is greater than 40 and remains unmeasured.
- **Sustained throughput** is measured (section 7b): 2455 msg/s over five minutes with no
  drops and no back-pressure signal. The `cdc_acm` throttling question is still open, since
  that is a different transport, but neither the adapter nor the vendor stack is the
  bottleneck at that rate.
- `cGetValue` selector `0x2f`, used in the vendor open path and returning 1, has unknown meaning.
- The leading `1` of `cSetPin`, the three-transfer `0xdb` preamble, `cInboundData` body+0, and
  table selectors other than 2 are unexplained.
- The vendor's own debug log could not be enabled; see the negative result in
  `docs/WINDOWS-FINDINGS.md`.

Additional kernel IOCTLs decoded in `kernel_labeled/00018b9c.c`:
0x55006000 returns USB configuration descriptor; 0x55006018 returns two 32-bit ones;
0x5500601c queries USB string index 3 in language 0x0409; 0x5500a004 enters a reset helper;
0x5500a00c issues vendor request 0xda. These are distinct from the file-create 0xdb operation
and are not prerequisites established for ordinary bulk messages.

## 9. Reproduction and evidence

- `analysis/ExtractSenders.java`: all 47 direct control-sender callers, C plus assembly.
- `analysis/ExtractOpenSequence.java`: selected functions and caller-reference indexes.
- `analysis/RefineFifoSignatures.java`: assembly-derived custom register storage; exports refined C.
- `analysis/ExtractInbound.java`: channel vtable candidates and callback decompilation. Multiple
  inheritance means entries need call-site checks; the inventory alone does not identify a callback.
- `analysis/ExtractKernel.java` and `LabelKernelWdf.java`: 55 kernel functions and WDF table labels.
- `analysis/decompiled/`: exported evidence; `analysis/disassembly/`: focused objdump verification.
- `analysis/SENDERS.md`: sender navigation index; `analysis/REPRODUCE.md`: commands and provenance.

The original DLL Ghidra project was read-only throughout. Corrected signatures and labels are
reproducible script operations, not permanent edits to that original project. The kernel driver
was imported into a separate project. No binary patching, firmware writing, or hardware I/O occurred.
