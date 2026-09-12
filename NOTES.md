# MongoosePro JLR — Linux driver research

> This is the informal running research log. For the cleaned-up wire protocol reference
> (opcode table, frame structure, open questions), see [`PROTOCOL.md`](PROTOCOL.md).

## Device
- Drew Technologies Inc. / OPUS IVS "MongoosePro JLR" (Jaguar/Land Rover) J2534 interface
- USB VID:PID = `18e1:0104`, serial `AOLHE0000003666A`
- Enumerates on Linux as CDC-ACM: `/dev/ttyACM0` (kernel `cdc_acm` driver, no custom driver needed for raw transport)
- USB interfaces: `02:02:00` (CDC control) + `0a:00:00` (CDC data)

## Vendor driver (in `vendor/driver/`)
- `monpj432.dll` — the actual J2534 "PassThru" function library (32-bit PE, unstripped, MFC-based)
- `MPConfigApp.exe` — vendor's device config GUI (uses PnP dropdown, doesn't detect device under Wine)
- `dtmonpro.sys` / `monpjaguar.inf` / `monpjaguar.cat` — Windows kernel driver package (WDF-based, won't load under Wine)
- Sourced from official installer: `vendor/installer/J2534_MongoosePro_JLR_x64.{zip,msi}` (OPUS IVS official S3 download host)

## Exported functions (standard J2534-1 API + Drew Tech extensions)
PassThruOpen, Close, Connect, Disconnect, ReadMsgs, WriteMsgs, StartPeriodicMsg,
StopPeriodicMsg, StartMsgFilter, StopMsgFilter, SetProgrammingVoltage, ReadVersion,
GetLastError, Ioctl, ReadDetails, SetIncomingMsgCallback, FirmwareUpdate, GetNextCarDAQ

## Key finding: no COM-port API used
`monpj432.dll` imports:
- `SETUPAPI.dll`: SetupDiGetClassDevsW / EnumDeviceInterfaces / GetDeviceInterfaceDetailW /
  OpenDeviceInterfaceW / GetDeviceInstanceIdW / GetDeviceRegistryPropertyW
- `KERNEL32.dll`: CreateFileW, ReadFile, WriteFile, DeviceIoControl

No `SetupComm`/`EscapeCommFunction`/COMM API at all. This means on Windows the DLL finds the
device via SetupAPI device-interface enumeration, opens it with plain `CreateFileW`, then talks
to it with raw `ReadFile`/`WriteFile` (+ occasional `DeviceIoControl`). The kernel driver
(`dtmonpro.sys`) is likely just handing back a raw byte pipe over the USB endpoints — the real
protocol intelligence lives in user-mode in the DLL.

**Working hypothesis:** the same "wireframe" protocol runs directly over `/dev/ttyACM0` on Linux,
since CDC-ACM gives us an equivalent raw byte pipe. If true, no Linux kernel driver is needed —
just a userspace client implementing the wire protocol.

## Protocol clues found via strings (unstripped symbols!)
- Custom framing class: `cVFrameFIFO_wireframe` ("wireframe" protocol), shared between USB and
  Bluetooth transports (see "BT Serial port" strings) — supports the transport-agnostic theory above
- Classic ISO9141/14230 K-line init: `DT_ISO_INIT_BAUD`, "5 Baud Init", `eFiveBaudInitFailed`
- Custom vendor ConnectFlags: `DT_SNIFF_MODE`, `CHECKSUM_DISABLED`, `K_LINE_ONLY`
- Named low-level commands (likely literal wire opcodes used in logging):
  `cWriteSerialNumber` / `cWriteSerialNumberResp`, `cJumpToFirmware` / `cJumpToFirmwareResp`
- Supports protocols: CAN, ISO14230, ISO15765, ISO9141, J1850PWM (from registry PassThru
  capability flags at install time)

## Ruled out
- **Wine + vendor GUI**: `MPConfigApp.exe` enumerates COM ports via PnP "Ports" class; Wine doesn't
  synthesize that, so the device (though present as COM33 in the registry) never appears in its list.
  Confirmed this is NOT a Linux permission issue — raw `open()` on `/dev/ttyACM0` works fine as the
  user account (in `uucp` group), and Wine's own serial autodetection found the device at boot.
- **OpenVehicleDiag** (open-source J2534 client, has a merged fix for this device's `PTOpen(NULL,...)`
  quirk from 2021): its GUI needs Direct3D-via-vkd3d-via-Vulkan under Wine, and this machine's Intel
  Sandy Bridge iGPU has no Vulkan driver support (mesa `anv` requires 8th-gen+). Not fixable via env
  vars; a software Vulkan (lavapipe) path wasn't found readily packaged on Arch.
- **FreeSSM**: known unresolved issue finding the MongoosePro J2534 library at all.
- **Macchina-J2534**: only supports Macchina M2/A0 hardware, unrelated to this device.

## Ghidra decompilation results (`analysis/decompiled/`)
Installed Ghidra (extra/ghidra), ran headless analysis + a custom Java `GhidraScript`
(`analysis/ExtractProtocol.java`, `analysis/ExtractByAddress.java`) against `monpj432.dll`, decompiling
the exported PassThru* functions plus every function referencing protocol-relevant strings.
Full outputs are in `analysis/decompiled/*.c`. Key findings:

### Complete wire protocol opcode table (from `strref_FUN_1003b080` — command-name lookup)
This is the full application-layer command set the PC and adapter exchange. Every response opcode
= request opcode | 0x8000.
```
0x00 cPrintDebugText      0x0e cTableRemoveEntry   0x104 cSetBoardID
0x01 cRespGeneral         0x0f cTableModifyEntry   0x105 cSetBoardLed
0x02 cRespString          0x10 cTableClear         0x106 cSyncClock
0x03 cOpenDevice          0x11 cIoctl              0x107 cGetStats
0x04 cGetDeviceConfiguration  0x12 cSetPin         0x108 cBoardSleep
0x05 cCloseDevice         0x13 cGetString          0x109 cGetBoardInfo
0x06 cOpenChannel         0x14 cSetData            0x10a cReflashBoard
0x07 cCloseChannel        0x15 cGetData            0x10b cWriteSerialNumber
0x08 cOutboundData        0x100 cEchoPacket        0x10c cUnprotectBootloader
0x09 cInboundData         0x101 cGetBoardStatus    0x10d cBrownoutIndication
0x0a cIndication          0x102 cResetBoard        0x111 cCheckCRN
0x0b cSetValue            0x103 cJumpToFirmware    0x112 cUpdateBTModule
0x0c cGetValue
0x0d cTableAddEntry
```
`cOpenDevice`/`cCloseDevice`/`cOpenChannel`/`cCloseChannel`/`cOutboundData`/`cInboundData` map
directly to J2534 `PassThruOpen`/`Close`/`Connect`/`Disconnect`/`WriteMsgs`/`ReadMsgs`.
`cTableAddEntry`/`RemoveEntry`/`ModifyEntry`/`Clear` map to J2534 message filters
(`StartMsgFilter`/`StopMsgFilter`). `cSetPin` = `PassThruSetProgrammingVoltage`.

### Frame structure (partially confirmed)
- **4-byte length prefix** precedes each frame on the wire — confirmed in `cVFrameFIFO::data_obtain`
  (`byaddr_1006c6e0`, the FIFO/ring-buffer frame reader): first 4 bytes pulled off the byte stream are
  read as `nBytesInThisFrame` and validated against buffer capacity ("Buffer is hosed..." error if too
  large).
- **Minimum valid frame = 12 bytes** — enforced in `strref_FUN_1006d810` (response-matching function):
  frames under 12 bytes are rejected as "too small for wire protocol".
- **Response correlation fields**: after parsing, the response-matcher compares a 2-byte field at
  (parsed-struct) offset 0 and another 2-byte field at offset 6 against the original request — almost
  certainly the opcode (offset 0, must equal `request_opcode | 0x8000`) and a sequence/transaction ID
  (offset 6). Note: this comparison happens on a *parsed* struct, not necessarily raw wire bytes 1:1 —
  the exact wire byte offsets still need confirmation.
- Checksum: not yet located. `CHECKSUM_DISABLED` exists as a connect flag (0x200), implying checksums
  are normally on by default and can be disabled — the algorithm itself hasn't been found yet (likely
  in one of the still-opaque `FUN_1006b670`/`FUN_1006b480`/`FUN_1006b450` ring-buffer helper functions).
- Standard SAE ISO9141/14230 5-baud slow-init logic (`strref_FUN_1002a700`) is implemented as a single
  high-level `cIoctl` (0x11) call to the adapter — meaning the time-critical K-line bit-banging happens
  in the adapter's own firmware, not over the wire protocol itself. Good news: this is public-standard
  behavior, nothing proprietary to reverse here.

### Corroborating context (from d5t5.com / Volvo VDASH community)
"Super J2534" and other cheap clones are described as unlicensed copies of the genuine Drew
Technologies Mongoose hardware, using the *same* Drew Technologies Windows driver. This confirms the
wire protocol isn't uniquely locked per unit — clone manufacturers have already replicated it well
enough to pass the official driver's handshake.

## Next step
The opcode table and rough frame skeleton are solid enough to start drafting a real spec, but nailing
exact byte offsets/order and the checksum algorithm has hit diminishing returns from pure static
analysis (deeply nested MFC/CRT-generated ring-buffer code with no remaining symbol names below
`cVFrameFIFO`). The highest-leverage next step would be **live USB traffic capture** (`usbmon` +
Wireshark) of the real Windows driver talking to the adapter — ideally connected to a vehicle, or at
least bench 12V power on the OBD-II pins — to confirm the frame layout empirically against known
opcodes/command names already identified here. Static analysis of the three ring-buffer helper
functions (`FUN_1006b670`, `FUN_1006b480`, `FUN_1006b450`) is the fallback if live capture isn't
available.

Device was not connected to a vehicle during this research — some behavior may only be
observable/testable with the adapter plugged into a car's OBD-II port (or bench 12V).

## Live USB capture session (2026-09-12)

Set up `usbmon` + Wireshark/tshark on this Linux box (Omarchy) and wrote a probe tool
(`analysis/probe.py`) to send hand-built candidate frames directly to `/dev/ttyACM0` and watch
both the raw serial response and the USB bulk traffic (capture saved to
`analysis/captures/probe_session_2026-09-12.pcapng`).

**Important scope note:** there is no working Windows/Wine environment that can actually drive
`monpj432.dll` against this device (Wine can't enumerate the vendor's device-interface GUID — see
"Ruled out" above), so this was *not* a capture of the real vendor driver's traffic. It was our own
probe tool talking to the adapter directly, checked against the reconstructed protocol hypothesis
in `PROTOCOL.md`.

### Confirmed
- **Transport hypothesis holds**: bytes written to `/dev/ttyACM0` go out on the wire completely
  unmodified as bulk-OUT packets on EP `0x01` — verified byte-for-byte in the capture (our 12-byte
  test frame `0c 00 00 00 00 01 01 00 00 00 00 00` appeared verbatim in the USB payload). No
  CDC-ACM line-discipline mangling, no extra framing.
- Every bulk-OUT transfer we sent completed with **URB status: Success (0)** at the USB level —
  the device's USB stack accepts the raw bytes cleanly regardless of content.
- Endpoints confirmed: bulk OUT `0x01` / bulk IN `0x82`, 64-byte max packet, full-speed (12 Mbps).
  A third endpoint, interrupt IN `0x83` (8-byte, 255ms interval) on the CDC control interface,
  exists but wasn't examined.

### Not confirmed / open
- **Zero application-layer responses** were observed for any of ~20 candidate request frames:
  `cOpenDevice` (0x003), `cEchoPacket` (0x100), `cGetBoardStatus` (0x101), `cGetBoardInfo` (0x109),
  `cGetDeviceConfiguration` (0x004) — each tried with both little- and big-endian field order, and
  both interpretations of whether the 4-byte length prefix includes itself. Also tried both 9600
  and 115200 baud (shouldn't matter for CDC-ACM bulk transport, but tested in case firmware gates
  on `SET_LINE_CODING`). No reply frame, no error/string response, not even at the raw USB level
  (no unsolicited bulk-IN data at any point, confirmed by passively listening with zero writes).
- This means either: the header field layout is still wrong (order/offsets/constants beyond what
  static analysis extracted), a checksum is required and unchecksummed frames are silently
  dropped, or some other required preamble/handshake wasn't reproduced.
- Went looking for the actual frame-serialization code in `analysis/decompiled/PassThruOpen.c`
  hoping to shortcut the guesswork — dead end. That function is pure Windows-side device discovery
  (matching `"MongoosePro JLR-####"` / `"USB\\..."` device path strings via SetupAPI), not wire
  framing. The real serializer is still buried in the unlabeled ring-buffer helpers
  (`FUN_1006b670`/`FUN_1006b480`/`FUN_1006b450`) mentioned above.

### Decision: paused
Given the risk that a wrong-but-plausible byte guess in further blind probing could alias a
destructive opcode (`cReflashBoard` 0x10a, `cWriteSerialNumber` 0x10b, `cUnprotectBootloader`
0x10c, `cJumpToFirmware` 0x103 are all one field-offset guess away from where we were probing),
decided to stop live probing here rather than keep guessing. Two viable paths forward, not yet
chosen between:
1. Deeper Ghidra decompilation targeting `FUN_1006b670`/`FUN_1006b480`/`FUN_1006b450` specifically,
   to find the real serializer/checksum before sending anything else.
2. Get access to a real Windows install (physical or VM with USB passthrough) so the actual vendor
   driver can talk to the device for a genuine reference capture.

### Capture tooling setup (now permanent, no sudo/pkexec needed going forward)
- `usbmon` kernel module auto-loads at boot via `/etc/modules-load.d/usbmon.conf`.
- udev rule `/etc/udev/rules.d/60-usbmon.rules` keeps `/dev/usbmon*` group-`wireshark`, mode 0640.
- User is a permanent member of `wireshark` (usbmon capture) and `uucp` (`/dev/ttyACM0`) groups.
- `dumpcap` has `cap_net_raw`/`cap_net_admin` file capabilities from the Arch `wireshark-cli`
  package, so capture works fully unprivileged once group membership is active in a shell.
- Reusable probe script: `analysis/probe.py` — sends a configurable sweep of opcode/endian/length
  variants over `/dev/ttyACM0` and prints whatever comes back; extend `OPCODES` there for further
  hypothesis testing once a better frame layout candidate is found.

## Deeper Ghidra dig on the ring-buffer helpers (2026-09-12, later same day)

Went back into Ghidra (`analysis/ExtractRingBuffer.java`, headless, output in
`analysis/decompiled/ringbuffer/`) specifically targeting the three previously-unlabeled helper
functions (`FUN_1006b670`/`FUN_1006b480`/`FUN_1006b450`) plus their callers and callees, hoping one
of them was the frame serializer/checksum.

**They aren't.** All three turned out to be generic circular-buffer plumbing with no
protocol-specific logic at all:
- `FUN_1006b450` — advance the FIFO read cursor by N bytes, wrapping at the buffer boundary,
  decrementing an "available bytes" counter.
- `FUN_1006b480` — advance/commit the write cursor by N bytes, same wraparound pattern, resets to
  head on hitting capacity.
- `FUN_1006b670` — memcpy-with-wraparound: copies bytes out of a circular source range into a
  linear destination buffer, recording `{base, tag, bytesWritten}` into an output struct.

### But this surfaced a genuinely new, useful function: `cVFrameFIFO::data_obtain`

One of the callers pulled in by the script, `FUN_1006c6e0`, turned out to be a **fully-named,
previously-undecompiled function** — confirmed by an embedded string literal
(`"cVFrameFIFO::data_obtain"`) passed to an exception constructor. This is the plain-response frame
reader, and reading through its logic (cross-checked against the raw `.asm` at
`analysis/decompiled/ringbuffer/caller_of_1006b480_FUN_1006c6e0_1006c6e0.asm`) gives a much sharper
picture of the frame header than we had before:

1. Reads **4 bytes**: `nBytesInThisFrame`. Validated as `available >= nBytesInThisFrame + 8` before
   proceeding — this is exactly where the documented 12-byte minimum frame size comes from
   (4-byte length field + this required 8 more bytes with `nBytesInThisFrame == 0`).
2. Reads **4 more bytes** ("field A") and copies them into the *caller-supplied* output buffter —
   i.e. this is data the higher-level code actually gets to see. Strong candidate: the combined
   2-byte opcode + 2-byte correlation/sequence field from the original hypothesis in `PROTOCOL.md`
   §3, now confirmed to be a single contiguous 4-byte block rather than two separately-handled
   2-byte fields.
3. Reads **4 more bytes** ("field B") into a purely local stack variable that is **never returned
   to the caller** — discarded (at least as far as this function's return value goes). Candidate:
   a checksum, or a reserved/padding field. This is where nBytesInThisFrame-worth of actual payload
   would presumably follow for opcodes that carry one, but this generic reader only handles the
   fixed 12-byte envelope — variable-length payload handling is presumably in the sibling function
   (`byaddr_1006d9b0`, the one requiring 20-byte minimum frames) that wasn't re-examined this pass.

### Hit a real wall, not just "haven't looked yet"
Tried to pin down exactly *where* field B is read from (to determine whether it's checksum-validated
somewhere we can't see, or genuinely just discarded) by tracing the raw assembly around the second
and third `CALL 0x1006b480` inside `data_obtain`. Both calls rely on a `this`-like pointer that's
loaded into `EAX`/`ECX` from a point **earlier in the function that Ghidra's own decompiler failed
to resolve** (shown as `in_EAX`/`unaff_EBX` in the pseudocode — Ghidra explicitly gives up tracking
these, not just "hasn't been decompiled yet"). This is a genuine dead end for headless/scripted
Ghidra analysis; going further would mean manual register-flow tracing inside the interactive
Ghidra GUI, which is a fundamentally slower, hands-on workflow rather than more automated
decompilation.

**Decision:** stopped here rather than push into manual GUI-based tracing. The frame-header
hypothesis is meaningfully sharper than before (see updated `PROTOCOL.md` §3), but the field-B
checksum question remains open, gated on either manual interactive Ghidra work or real hardware
ground truth (live Windows driver capture, still not available — see above).
