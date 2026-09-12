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
| Windows driver files | `monpj432.dll` (32-bit, protocol logic), `dtmonpro.sys` (kernel driver, transport behavior not yet traced) |

## 2. Transport

The vendor DLL opens the device via Windows `SetupDiEnumDeviceInterfaces` → `CreateFileW`, then
communicates with plain `ReadFile`/`WriteFile` (+ occasional `DeviceIoControl`). No Windows COMM
API (`SetupComm`, `EscapeCommFunction`) is used. **Working hypothesis:** the kernel driver is just
handing back a raw byte pipe over the USB CDC endpoints — equivalent to what Linux's `cdc_acm`
already exposes as `/dev/ttyACM0`. If true, no custom Linux kernel driver is needed; a userspace
client can talk directly over the tty. **Partially tested:** Linux probe bytes reached USB bulk OUT unchanged, but no application
responses or vendor-driver exchanges were captured. Windows transport equivalence remains unverified.

The internal framing class is named `cVFrameFIFO_wireframe` ("wireframe" protocol), and is shared
between USB and Bluetooth transports (per string `"BT Serial port has seen:"`), meaning the
application-layer protocol below is transport-agnostic.

## 3. Outgoing control framing (assembly traced, not hardware validated)

`FUN_1006e180` builds the following buffer and passes it through `FUN_1006a420` to
`WriteFile`. This supersedes the previous 32-bit-length-prefix hypothesis.

| Offset | Size | Value |
|---|---|---|
| 0 | 2 | Request-body byte count N, little-endian; excludes this 4-byte prefix |
| 2 | 2 | N XOR 0x51E6, little-endian |
| 4 | N | Caller-provided request body, copied unchanged |

Evidence in `analysis/disassembly/`:
- `send_control.asm`: at `1006e1d8` the low 16 bits of the length are stored; at
  `1006e1db`–`1006e1eb` the second word is computed by XOR with `[context+0x4028]`.
  `1006e1ef` calls the memcpy thunk at `10074120` (import slot `1007f250`).
  At `1006e21a`–`1006e221`, buffer and N+4 are passed to `FUN_1006a420`.
- `framing_context.asm`: constructor `1006dd43` initializes a subobject at context+4.
  `framing_init.asm`: `1006ae24` loads 0x51E6 and `1006ae51` stores it at
  subobject+0x4024, i.e. context+0x4028. This establishes the initialization value;
  writes through aliases have not been exhaustively audited.
- `write_file.asm`: `1006a473` calls the WriteFile import at `1007f034`, using the
  original buffer pointer and byte count. This wrapper adds no bytes or checksum.
- `framing_receive_check.asm`: the receive parser also checks the two prefix words
  using XOR with the initialized constant, then waits for N+4 bytes.
- `pe_imports.txt` identifies WriteFile and memcpy import slots.

This XOR validates the length field; it is not a checksum over the body. The traced control
send path adds no body checksum. Kernel-driver transformations and device acceptance remain
unverified; the alternate transport branch has not been fully traced.

### Example caller: cIoctl for 5-baud initialization

`ioctl_send.asm` traces `FUN_1002a700` into `FUN_1006e180` at `1002a7be`.
Accounting for the push at `1002a773`, the body is 17 bytes:

| Body offset | Size | Observed source/value |
|---|---|---|
| 0 | 2 | Caller context word at +0x0c; likely channel identifier |
| 2 | 2 | Global word at 0x10081730 (zero in the supplied DLL) |
| 4 | 2 | 0x0011, cIoctl opcode |
| 6 | 2 | Zero-extended incrementing byte sequence; zero is skipped |
| 8 | 2 | Zero |
| 10 | 2 | Not explicitly initialized in the inspected instruction range |
| 12 | 4 | Zero |
| 16 | 1 | Caller argument (initialization address) |

The resulting prefix is `11 00 f7 51` and WriteFile receives 21 bytes. This is a static
construction example, not an instruction to send it. Field semantics and the apparently
uninitialized bytes need further investigation. Other opcodes' bodies are not established here.

### cOpenDevice construction (2026-09-13)

`FUN_1003ea00` builds a 12-byte body and calls `FUN_1006e180` at `1003ea9c`.
With the initialized XOR constant, WriteFile receives 16 bytes:

```text
0c 00 ea 51  01 00 00 00  03 00 SS 00  00 00 ?? ??
```

`SS` is the current nonzero byte sequence, widened to 16 bits; the context counter is
incremented afterward. `??` denotes stack bytes not explicitly initialized in this function,
not known zeros. No opcode-specific payload follows this 12-byte body.

| Body offset | Size | Value/source |
|---|---|---|
| 0 | 2 | 1, global word at 0x100806c8; meaning unresolved |
| 2 | 2 | 0, global word at 0x100806c4; meaning unresolved |
| 4 | 2 | 3, cOpenDevice |
| 6 | 2 | Nonzero sequence byte widened to a word |
| 8 | 2 | Zero |
| 10 | 2 | No explicit initialization in the function before send |

Evidence: `analysis/disassembly/open_device.asm` and `open_device_constants.txt`.
The body begins at the pre-push stack offset +0x10; after pushing length 12, the
pointer is formed as ESP+0x14. This stack adjustment establishes the offsets above.
The response wait calls `FUN_1006d9b0` at `1003ead0` with a 128-byte buffer capacity;
its existing minimum-returned-size check is 20 bytes.

The caller at `1005182c` invokes `FUN_1003e820` before `FUN_1003ea00` at `10051833`.
That preceding routine sends opcode 0x0103 (`cJumpToFirmware`) and waits for a response.
See `open_device_caller.asm` and `pre_open_jump.asm`. Thus this observed open path has
an earlier command; do not assume cOpenDevice is the first transaction or SS is 1.
Whether that preceding command is required in the device's current state is unverified.

Next: trace the preceding transport initialization and response handling to establish the
full opening sequence. These bytes are static evidence, not a hardware-validated probe.

### 3.1 Evidence audit (2026-09-12; supersedes earlier framing claims)

The three helpers `FUN_1006b670`/`FUN_1006b480`/`FUN_1006b450` are generic FIFO operations,
not an identified wire serializer. The former framing diagram and 32-bit USB length-prefix claim were unsupported:
internal FIFO records may have their own framing. The send-path trace above is separate evidence.

Re-reading `byaddr_1006c6e0_FUN_1006c6e0.c` shows:
- The first copy reads 4 bytes into `local_478`; available bytes must be at least `local_478 + 8`.
- The middle copy into the caller's buffer passes `param_4`, not a literal 4, to the copy helper.
  Its range depends on unresolved cursor-helper signatures. Three fixed 4-byte reads are not established.
- A later copy reads 4 bytes into `local_45c`; its exact position and purpose remain unresolved.
  Calling this USB bytes 8–11 or a checksum is unsupported.
- The successful path returns the saved record length. Both response matchers (`1006d810` and
  `1006d9b0`) call this same reader and check its returned size against 12 or 20 bytes. These
  checks do not establish a fixed 12-byte envelope or a separate payload reader.

Unresolved decompiler signatures warrant assembly tracing and prototype correction; they do not
prove that further headless analysis is impossible. The subsequent send-path trace above distinguishes outgoing transport framing from this queue evidence.

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

1. **Validate the traced control prefix and map remaining body fields** — DLL WriteFile bytes
   are now traced; real vendor USB traffic and successful Linux responses remain unavailable.
2. **Confirm framing end to end** — the control send path adds a length XOR check, with no
   body checksum. Kernel-driver behavior remains unverified. The channel-level
   `CHECKSUM_DISABLED` flag does not establish a USB checksum.
3. **Payload structure** for `cOutboundData`/`cInboundData` (the actual diagnostic message frames)
   and `cTableAddEntry` (message filter definitions) — not yet examined.
4. **`cIndication`** — likely the async/unsolicited notification opcode used for `PassThruReadMsgs`
   callback delivery; not yet examined.

### Live probing result (2026-09-12) — raw output confirmed, framing unresolved

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
  framing. The subsequently examined ring-buffer helpers are generic FIFO operations; the actual
  serializer remains unidentified.
- **Paused deliberately** rather than continuing blind guessing: a wrong-but-plausible opcode value
  could alias a destructive command (`cReflashBoard` 0x10a, `cWriteSerialNumber` 0x10b,
  `cUnprotectBootloader` 0x10c, `cJumpToFirmware` 0x103 are all one field-offset guess away from the
  region being probed).

**Recommended next step:** either (a) trace initialization preceding cJumpToFirmware/cOpenDevice
and response handling before sending anything else, or (b) get access to a real Windows install (physical or VM with USB passthrough) to
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

## 10. Ghidra opening-sequence follow-up (2026-09-13)

Ghidra 12.1.2 successfully exported nine functions from the existing project using
`-readOnly -noanalysis`; the project database was not modified. Reusable extraction script:
`analysis/ExtractOpenSequence.java` (output directory followed by target addresses).

The observed path in `FUN_100500c0` is:

1. For a newly created context, `FUN_1006df00` selects the transport initializer.
   Its hardware branch calls `FUN_10069f00` (`cHardwareStream::initialize`). This enumerates
   the device interface, opens it with `CreateFileW` for exclusive read/write access using
   overlapped I/O, resets four events, and sets its running flag. No explicit WriteFile or
   DeviceIoControl call appears in that initializer. Kernel behavior on open is not established.
   The outer initializer then sets its running flag and calls `DAT_100a3170`, still unresolved.
2. `FUN_1003e820` sends cJumpToFirmware (0x0103), waits for a response, and accepts status
   **0 or 7**. Other statuses take an exception path; the meaning of 7 is not established here.
3. `FUN_1003ea00` sends cOpenDevice (0x0003), waits for a response, and accepts status **0**.
   Status **0x020a** produces the explicit message “No voltage on vehicle connector, connect
   cable or check circuit”; other nonzero statuses also take an exception path. This is evidence
   that missing vehicle power can prevent opening, not proof of the cause of earlier silent probes.
4. After opening, the caller sets context+0xc23c to 1 and calls `FUN_1003f9c0`, which sends
   cGetBoardInfo (0x0109), waits for a response, and rejects nonzero status.

Response dispatcher `FUN_10043610` reads opcode at incoming-body+4 and recognizes 0x8003,
0x8103 and 0x8109 among responses routed to `FUN_1006c5e0`. The precise queue transformation
before the existing response matcher is still untraced; do not equate its offsets with raw USB
without following that insertion path. The 12-byte request bodies' final two bytes still have
no explicit initialization in the inspected construction functions.

Sources: `analysis/decompiled/open_sequence/`, `open_transport/`, and `usb_open/`, with
references.txt call-site indexes and `analysis/ghidra_*open*.log` execution logs.
No device commands were sent. Next bounded step: identify DAT_100a3170 and trace response
queue insertion to reconcile body offsets with the response matcher.
