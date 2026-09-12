# MongoosePro JLR — Wire Protocol Documentation

Status: reverse-engineered via static analysis of the vendor's Windows J2534 DLL. Not yet
validated against live USB traffic. Confirmed facts are cited to the decompiled source file
they were derived from (`analysis/decompiled/*.c`); everything else is marked as a hypothesis.

## 1. Device identification

| Property | Value |
|---|---|
| Vendor | Drew Technologies Inc. (now OPUS IVS) |
| Product | MongoosePro JLR (Jaguar/Land Rover) |
| USB VID:PID | `18e1:0104` |
| Linux enumeration | CDC-ACM, `/dev/ttyACM0` (interfaces `02:02:00` control + `0a:00:00` data) |
| Windows driver files | `monpj432.dll` (32-bit, protocol logic), `dtmonpro.sys` (kernel driver, raw byte pipe only) |

## 2. Transport

The vendor DLL opens the device via Windows `SetupDiEnumDeviceInterfaces` → `CreateFileW`, then
communicates with plain `ReadFile`/`WriteFile` (+ occasional `DeviceIoControl`). No Windows COMM
API (`SetupComm`, `EscapeCommFunction`) is used. **Working hypothesis:** the kernel driver is just
handing back a raw byte pipe over the USB CDC endpoints — equivalent to what Linux's `cdc_acm`
already exposes as `/dev/ttyACM0`. If true, no custom Linux kernel driver is needed; a userspace
client can talk directly over the tty. **Not yet empirically confirmed** — no live capture has been
done.

The internal framing class is named `cVFrameFIFO_wireframe` ("wireframe" protocol), and is shared
between USB and Bluetooth transports (per string `"BT Serial port has seen:"`), meaning the
application-layer protocol below is transport-agnostic.

## 3. Frame structure (partially confirmed)

```
+----------------+------------------+-----------------------+
| 4 bytes        | 2 bytes          | 2 bytes                |
| frame length   | opcode           | sequence / correlation |
| (uint32)       | (see §4)         | id                     |
+----------------+------------------+-----------------------+
| ... payload (opcode-dependent) ...                         |
+--------------------------------------------------------------+
```

Confirmed:
- **4-byte length prefix** precedes every frame. Source: `cVFrameFIFO::data_obtain`
  (`analysis/decompiled/byaddr_1006c6e0_FUN_1006c6e0.c`) — reads 4 bytes off the ring buffer first,
  as `nBytesInThisFrame`, validated against buffer capacity before proceeding ("Buffer is hosed..."
  error path otherwise).
- **Minimum valid frame** is 12 bytes for simple control responses
  (`analysis/decompiled/strref_FUN_1006d810_1006d810.c`, rejects `< 0xc`), or 20 bytes for
  responses expected to carry a data payload
  (`analysis/decompiled/byaddr_1006d9b0_FUN_1006d9b0.c`, rejects `< 0x14`).
- **Response correlation**: response-matching code in both of the above compares a 2-byte field at
  (parsed) offset 0 and another 2-byte field at offset 6 against the original request before
  accepting a frame as the matching response. Field at offset 0 is presumed to be the opcode
  (`request_opcode | 0x8000`); field at offset 6 is presumed to be a per-request sequence id.
- **Header construction on send** (seen identically in three separate call sites building an
  outgoing `cIoctl` (0x11) frame — `analysis/decompiled/strref_FUN_1002a700_1002a700.c`,
  `analysis/decompiled/strref_FUN_1001b800_1001b800.c`): the header is built from:
  - a 2-byte opcode field (`0x11` = `cIoctl` in the observed cases)
  - a 2-byte field holding a fixed global constant (differs by call site — possibly a sub-command
    or protocol-version tag, not yet identified)
  - a 1-byte incrementing sequence counter (persisted per-device-instance, at a fixed struct offset
    `+0xc288` relative to a device context pointer)
  - a 2-byte reserved/zero field
  - followed by opcode-specific payload bytes

**Not yet found:** exact wire byte order (the struct-offset evidence above is post-parsing, not
guaranteed to be identical to the raw bytes on the wire), and the checksum algorithm.

### 3.1 Sharper header structure, from `cVFrameFIFO::data_obtain` (2026-09-12)

`FUN_1006b670`/`FUN_1006b480`/`FUN_1006b450` (previously listed as unidentified) turned out to be
generic circular-buffer plumbing (cursor-advance-with-wraparound, wraparound memcpy) — not the
serializer. But tracing their caller `FUN_1006c6e0`, confirmed by an embedded string literal to be
`cVFrameFIFO::data_obtain` (the plain-response reader), gives a firmer header layout. Reading a
plain (non-data-carrying) response frame consumes exactly 12 bytes, structured as three consecutive
4-byte reads:

```
+----------------+------------------+------------------------+
| 4 bytes        | 4 bytes          | 4 bytes                |
| nBytesInThisFrame | field A       | field B                |
| (validated:    | (returned to     | (read into a local var,|
|  available >=  |  the caller)     |  never returned —      |
|  N + 8)         |                  |  candidate: checksum   |
|                 |                  |  or reserved)          |
+----------------+------------------+------------------------+
```

- **Field A** (bytes 4–7) is copied into the caller-supplied output buffer as a single contiguous
  4-byte block — likely the combined opcode (2 bytes) + sequence/correlation id (2 bytes) from the
  §3 hypothesis above, now confirmed to move as one unit rather than two independently-handled
  fields.
- **Field B** (bytes 8–11) is read but **discarded within `data_obtain` itself** — never appears in
  the function's return value. Best guess is a checksum or reserved field, but this is unconfirmed.
- This 12-byte reader is for the generic/plain response case. The sibling function requiring a
  20-byte minimum (`byaddr_1006d9b0` — presumably payload-carrying responses) wasn't re-examined in
  this pass; the extra 8 bytes there most likely cover the `nBytesInThisFrame`-sized payload for
  opcodes that carry data, but that's not yet confirmed either.

**Wall hit:** determining exactly where field B is sourced from (to settle checksum-vs-discarded)
requires tracing a `this`-like pointer that Ghidra's own decompiler failed to resolve across two
calls to the write-cursor helper (`FUN_1006b480`) inside `data_obtain` — shown as `in_EAX`/
`unaff_EBX` in the decompiled C, meaning Ghidra's automated analysis gave up, not just "hasn't gotten
to it yet". Further progress here needs manual register-flow tracing in the interactive Ghidra GUI
(a much slower, hands-on workflow), or real hardware ground truth. See `NOTES.md`'s "Deeper Ghidra
dig on the ring-buffer helpers" entry for the full trace.

## 4. Command/response opcode table

Source: `analysis/decompiled/strref_FUN_1003b080_1003b080.c` (a pure opcode→name lookup function,
fully decompiled with no ambiguity). Every response opcode = request opcode `| 0x8000`.

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

### Mapping to the J2534 API

| J2534 export | Wire opcode |
|---|---|
| `PassThruOpen` | `cOpenDevice` |
| `PassThruClose` | `cCloseDevice` |
| `PassThruConnect` | `cOpenChannel` |
| `PassThruDisconnect` | `cCloseChannel` |
| `PassThruWriteMsgs` | `cOutboundData` |
| `PassThruReadMsgs` | `cInboundData` (async, via `cIndication`?) |
| `PassThruStartMsgFilter` | `cTableAddEntry` |
| `PassThruStopMsgFilter` | `cTableRemoveEntry` |
| `PassThruIoctl` | `cIoctl` |
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

## 7. Standard protocol behavior (not proprietary, no need to reverse further)

ISO9141/ISO14230 5-baud slow-init handshakes (`analysis/decompiled/strref_FUN_1002a700_1002a700.c`,
`analysis/decompiled/strref_FUN_1001b800_1001b800.c`) are implemented as a single high-level
`cIoctl` (0x11) call to the adapter — the time-critical K-line bit-banging (send 0x33 at 5 baud,
receive sync byte 0x55, receive two key bytes, send inverted key byte, receive inverted address)
happens in the adapter's own onboard firmware, not over the wire protocol. This is public SAE
standard behavior, already documented for decades — no reverse-engineering needed here.

## 8. Open questions / next steps

1. **Exact raw byte order and offsets** of the frame header — current evidence comes from parsed
   in-memory structs, not confirmed against actual wire bytes.
2. **Checksum algorithm** — location unknown; `CHECKSUM_DISABLED` flag implies one exists.
3. **Payload structure** for `cOutboundData`/`cInboundData` (the actual diagnostic message frames)
   and `cTableAddEntry` (message filter definitions) — not yet examined.
4. **`cIndication`** — likely the async/unsolicited notification opcode used for `PassThruReadMsgs`
   callback delivery; not yet examined.

### Live probing result (2026-09-12) — transport confirmed, header layout still wrong

Built a probe tool (`analysis/probe.py`) and captured USB traffic (`usbmon` + tshark,
`analysis/captures/probe_session_2026-09-12.pcapng`) while sending hand-built candidate frames
directly to `/dev/ttyACM0`. Note: this was our own probe talking to the adapter, **not** a capture
of the real Windows driver — no working Windows/Wine environment is available to produce that (see
`NOTES.md` "Ruled out"). Results:

- **Transport hypothesis confirmed**: bytes written to `/dev/ttyACM0` appear byte-for-byte identical
  on the wire as bulk-OUT packets (EP `0x01`), no CDC-ACM mangling. Every transfer completed with
  `URB status: Success (0)` at the USB level regardless of content.
- **Zero application-layer responses** to ~20 variants of `cOpenDevice`/`cEchoPacket`/
  `cGetBoardStatus`/`cGetBoardInfo`/`cGetDeviceConfiguration` (both byte orders, both length-prefix
  interpretations, 9600/115200 baud) — no reply frame, no error string, no unsolicited data even
  when passively listening. The header layout in §3 above is therefore still unconfirmed; something
  about field order, an extra constant, or a required checksum is missing.
- Checked `analysis/decompiled/PassThruOpen.c` hoping it would contain the frame serializer — it
  doesn't; that function is pure Windows-side device discovery (SetupAPI name matching), not wire
  framing. The real serializer is still inside the unlabeled ring-buffer helpers
  (`FUN_1006b670`/`FUN_1006b480`/`FUN_1006b450`).
- **Paused deliberately** rather than continuing blind guessing: a wrong-but-plausible opcode value
  could alias a destructive command (`cReflashBoard` 0x10a, `cWriteSerialNumber` 0x10b,
  `cUnprotectBootloader` 0x10c, `cJumpToFirmware` 0x103 are all one field-offset guess away from the
  region being probed).

**Recommended next step (undecided as of this writing):** either (a) targeted Ghidra decompilation
of the three ring-buffer helper functions above to find the real serializer/checksum before sending
anything else, or (b) get access to a real Windows install (physical or VM with USB passthrough) to
capture the genuine vendor driver's traffic. USB capture tooling on this Linux box is fully set up
and permanent (no sudo/pkexec needed) — see `NOTES.md` for the setup details.

## 9. Source material index

- `vendor/driver/monpj432.dll` — the analyzed binary (32-bit PE, unstripped C++ symbols)
- `analysis/ghidra_project/` — Ghidra project with full auto-analysis
- `analysis/ExtractProtocol.java` — Ghidra script: decompiles named J2534 exports + all functions
  referencing protocol-relevant strings
- `analysis/ExtractByAddress.java` — Ghidra script: decompiles specific functions by address (for
  following call chains)
- `analysis/decompiled/*.c` — all decompiler output (26 files)
- `analysis/ExtractRingBuffer.java` — Ghidra script: decompiles the ring-buffer helper functions
  plus their callers/callees, with a raw `.asm` dump alongside each as a decompiler-failure fallback
- `analysis/decompiled/ringbuffer/` — output of the above, including `cVFrameFIFO::data_obtain`
- `analysis/probe.py` — Linux-side probe tool: sends candidate frames to `/dev/ttyACM0` and prints
  the response, for empirical testing of frame-layout hypotheses against the real firmware
- `analysis/captures/` — `usbmon`/tshark USB capture(s) from probe sessions
- `NOTES.md` — informal running research log (this file is the cleaned-up reference derived from it)
