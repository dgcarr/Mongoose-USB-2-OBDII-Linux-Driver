# Implementation and validation status

2026-09-13 Australia/Sydney (captures use 2026-09-12 UTC).
This is an experimental device-management and CAN channel-lifecycle library, **not a complete J2534 driver**.
The 04.04 version string identifies the target interface, not a compliance claim.

## Hardware results

Adapter 18e1:0104, serial AOLHE0000003666A, USB only; user confirmed no vehicle attached.

- Vendor control 0xdb value 1 succeeded; Echo and GetBoardInfo returned valid frames.
- **0xdb is not required.** After `USBDEVFS_RESET` (clearing any latched firmware state),
  Echo, GetBoardInfo and GetString all succeeded over plain `/dev/ttyACM*` under `cdc_acm`
  with no control transfer sent. Frames parsed with zero resync slides at lengths
  12/44/168/604 bytes. Reproduce: `analysis/probes/reset_test.py`.
- GetString returned `AOLHE0000003666A`, byte-identical to the USB `iSerial` descriptor.
- Response+16 is a device microsecond counter; `cOpenDevice` returns it as zero, i.e. the
  command resets the device clock.
- Failed commands return NUL-terminated ASCII at body+20, e.g. status 0x0203
  `cGetValue: Invalid message length.` and status 0x0007
  `ProcessCommand: Unknown/Unhandled Command`.
- `cGetValue` requires a body of exactly four bytes (the selector).
- `cGetDeviceConfiguration` (0x04) is not implemented by this firmware (status 7).
- Response opcodes 0x8101 and 0x8107 observed, extending the 0x8100 note.
- `cResetBoard` returns status 0 with text `FW RESET!!` and restarts the adapter **into the
  bootloader** (status 2). `cJumpToFirmware` returns it to firmware (status 1). Both
  transitions exercised repeatedly and are non-destructive; this is the vendor open path.
- The bootloader answers Echo/GetBoardStatus/GetBoardInfo/GetString/CheckCRN but rejects
  GetStats and OpenDevice with status 1 `eNotSupported` (`Invalid or Unhandled command type`).
- Board header resolved: body+8 board type 5, +9 board status, +20 bootloader version,
  +24 firmware version — the last two match the earlier selector 0x2a/0x2b readings.
- `cSetBoardLed` and `cSyncClock` return no response (fire-and-forget).
- Not executed: `cReflashBoard`, `cUnprotectBootloader`, `cWriteSerialNumber`,
  `cUpdateBTModule`, `cSetBoardID`, `cBoardSleep`.
- StartFirmware/OpenDevice/GetBoardInfo/CloseDevice succeeded.
- StartFirmware status 7 includes the text `Board already in firmware`.
- Cleanup control 0xdb value 0 succeeded; both interfaces returned to cdc_acm.
- Zero-initialized body bytes 10–11 were accepted for these commands.
- Firmware selector 0x2b returned 0x01011000 (1.1.16.0).
- Bootloader selector 0x2a returned 0x01010800 (1.1.8.0).
- Battery selector 3 returned zero; programming-voltage measurement selector 2
  returned 1914 raw millivolts. No voltage was applied by our software. Measurement
  accuracy is not independently verified; the adapter was disconnected from a vehicle.
- GetValue response+20 echoes the selector; response+24 contains the value.
- Open succeeded without vehicle power on this unit. The vendor's missing-voltage
  error path must not be interpreted as a universal open prerequisite.

Evidence: `analysis/captures/linux-*.trace`. These are timestamped application
transfer logs from the libusb backend, **not usbmon pcaps**. A cdc_acm trace has a
smaller vocabulary -- `OUT` and `IN` only, with no `CONTROL`/`CONTROL_RESULT_n` and no
`INTERRUPT`, since the CDC notification endpoint is consumed by the kernel. Original traces are retained. Replay fixtures
strip only timestamps and control records; no successful response was synthesized.
The earlier `linux-inspect-...trace` contains only open/close, before the inspection
mode was corrected; `linux-values-...trace` is the actual value-query capture.

## Transports

The **cdc_acm backend is the default** and needs no libusb, no interface detaching, no
root and no udev rule for access. The libusb backend remains selectable with `usb:` as
a fallback while vehicle traffic is unvalidated. `-DMONGOOSE_LIBUSB=OFF` builds and
passes the full suite with no libusb present.

Hardware results over cdc_acm, adapter only, no vehicle -- directly comparable with the
libusb figures above:

- `--list` resolves `/dev/ttyACM3 serial=AOLHE0000003666A access=ok`, correctly ignoring
  three unrelated CDC-ACM devices on the same host.
- `--discover`, `--open-close` and `--inspect` all succeed. StartFirmware returns status
  7 `Board already in firmware`; firmware selector 0x2b returns `0x01011000` (1.1.16.0)
  and bootloader 0x2a returns `0x01010800` (1.1.8.0) -- identical to the libusb run.
- Traces contain only `OUT`/`IN` records and no decoder resync.
- 40 consecutive open/close cycles, zero failures, ~100 ms each.
- Command round-trip measured at ~0.15 ms, well inside the 1 ms minimum budget.
- ASan/UBSan and ThreadSanitizer builds are clean, including live hardware runs that
  exercise the reader thread and the `stop()` eventfd wakeup.
- Exclusivity: `flock` plus `TIOCEXCL` means a second instance, and any unrelated
  opener such as `dd`, fails with `EBUSY` -> `ERR_DEVICE_IN_USE`; the lock clears on
  close with no stale state.
- **Not measured:** `cdc_acm` throttling under sustained inbound traffic. The libusb
  path used two outstanding 8 KB URBs; the tty path goes through the n_tty flip buffer,
  which throttles rather than drops when the reader lags. Vehicle traffic is where this
  would first matter, so parity is not claimed.
- **Not exercised:** the partial-write retry loop. No read-only opcode produces a frame
  near the 0x1800 limit, so the loop ships covered only by inspection. A single
  6144-byte write was observed to complete in one call on an idle buffer.
- The two backends do not mutually exclude each other; see README.

## Capability matrix

| Feature | Static evidence | Implemented | Hardware validation |
|---|---|---|---|
| USB ownership, framing, startup/cleanup | Yes | Yes | USB-only success |
| Open/Close | Yes | Yes | USB-only success |
| ReadVersion | Yes | Yes | Firmware value retrieved |
| READ_VBATT / READ_PROG_VOLTAGE | Yes | Yes | Raw values retrieved; accuracy pending |
| ABI and GetLastError | Standard interface | Yes | Native client / offline tests |
| CAN Connect/Disconnect | Captured vendor frames | Yes | Linux USB-only success; no vehicle/12 V |
| CAN message I/O | Partial | No | Windows reference only |
| ISO15765 | Captured host reassembly / firmware flow control | No | Windows reference only; timing variations pending |
| K-line / J1850PWM | Partial | No | Suitable hardware required |
| Filters / periodic / configuration | Partial | No | Pending |
| Programming-voltage output | Partial | No | Pending electrical validation |

All 14 core exports exist. CAN Connect/Disconnect is implemented; other protocols
remain unsupported. One CAN-family resource is reserved per device, with a second
CAN or ISO15765 connection refused locally. Message/filter/periodic APIs reject
invalid IDs and report ERR_NOT_SUPPORTED for live channels. Unsupported IOCTLs and
voltage output never report success. The Linux driver has sent no bus messages or
firmware writes; the Windows reference harness has exercised OBD requests.

## Tests

CTest covers frame fragmentation, concatenation, invalid headers, bounds, randomized
round trips, general-response matching, unrelated indications, short responses,
sequence quarantine, timeout poisoning, cancellation, unplug notification, short
writes, nonzero write budgets, transport root-cause preservation, concurrent
command serialization, replay mismatches, C ABI layout, symbol exports, and 100
**synthetic** lifecycle cycles. Real discovery/open/inspection captures are replayed
separately through the diagnostic executable. The pre-write deadline-expiry branch
is inspected, not deterministically tested. The channel suite tests capture-derived
API setup/teardown, wrong-source rejection, argument validation, stale handles,
rollback, teardown rejection, real response timeout, short writes, unplug and
synchronized competing lifecycle calls. Every scripted wire exchange must be consumed.

All eight CTest entries pass in normal, ASan/UBSan and no-libusb builds after this
change. Earlier Clang/ThreadSanitizer/fuzzer results below describe the preceding
baseline, not a fresh run of the channel implementation.

The preceding baseline passed under GCC, Clang, ASan/UBSan and ThreadSanitizer.
The `MONGOOSE_FUZZ` libFuzzer target was run over the framing codec for roughly
6.2 million executions seeded from the captured frames, with no crash, leak or
sanitizer finding; that exercises the only code that parses untrusted wire data.

Sequence numbers are held for ten seconds before reuse. A response timeout or an
ambiguous write requires reopening the session, because a late or partially delivered
frame could otherwise be matched to a later command. A command whose deadline expires
**before** anything is written does not: nothing reached the wire, so only the sequence
slot is lost and the session stays usable. The write budget is rounded up to whole
milliseconds rather than truncated, so a caller with a sub-millisecond remainder still
reaches the adapter and no backend is ever passed a zero timeout. Zero is out of
contract for both: libusb treats it as no timeout at all, `poll(2)` as expire
immediately. `core_tests` now asserts the transport sees at least 1 ms. Hardware bounds on delayed duplicate
responses are still unknown. No firmware-generation marker has been established.

## Windows reference captures (2026-09-13)

The vendor driver was captured on Windows 11 against
the adapter and the 2017 Volvo XC60 D5 AWD with the engine running: device
lifecycle, `ReadVersion`, error paths, exclusivity, CAN and ISO15765 `Connect`
across three bauds and both flag settings, a wildcard pass filter over live bus
traffic, a flow-control filter, and OBD-II mode 01 and mode 09 requests.

That settles channel addressing, the two open-channel arguments, pin routing,
body+10 semantics and filter-ID mapping — see `PROTOCOL.md` section 7b and
`docs/WINDOWS-FINDINGS.md`. `core_tests` now checks our frame builder against real
vendor bytes, so the codec is validated against the vendor rather than against our
own probe output.

Two results worth carrying forward. `READ_VBATT` reads ~14.1 V with the engine
running, so the Linux bench `0` from `cGetValue` selector 3 was a no-vehicle
reading rather than a broken selector. And no destructive opcode appears anywhere
in the capture corpus.

## Linux CAN lifecycle acceptance (2026-09-13)

User confirmed the adapter was connected by USB only, with no car or external 12 V.
The production shared library completed Open, ReadVersion, three CAN
Connect/Disconnect pairs, and Close, all with status zero. The pairs were
500000 flags 0, 250000 flags 0, and 500000 flags CAN_29BIT_ID; channel handles
were 2, 3 and 4 within the same process. Each Connect emitted OpenChannel then
SetPin to 0x0501, and each Disconnect emitted CloseChannel to that node.

Evidence: `analysis/captures/linux-can-lifecycle-20260913T101951Z.trace`, with matching
`.strace`, `.log` and `.txt` metadata. These are tty syscall bytes captured with
strace, not usbmon packets. Reproduce with
`build/mongoose-client serial:SERIAL --can-lifecycle`. No bus transmission is
performed. This is channel setup acceptance, not vehicle communication validation.

## Next blocking work

1. Implement CAN pass filters and receive queues; first resolve the CAN filter
   type word against vendor construction/captures, rather than treating it as
   ISO15765 TxFlags. D2 establishes opaque filter handles and at least 40 filters,
   but not maximum capacity.
2. Validate Linux receive throughput and back-pressure on a busy bus. Windows D4
   supplies a five-minute ~2455 msg/s baseline with no reported overflow, not
   proof of zero loss or Linux transport parity.
3. Add CAN transmit, then ISO15765 host reassembly using the captured firmware
   flow-control split. STmin, block size and N_Bs variations remain untested.
4. Complete remaining protocol engines, periodic messages and IOCTLs with suitable
   vehicles/fixtures. C3/C4 are settled for this adapter: only one CAN-family
   channel can be open; further chan-field semantics cannot be inferred here.
5. Run 100 hardware cycles and a one-hour diagnostic soak once bus support exists.
   Three USB-only channel cycles and synthetic lifecycle tests do not satisfy these gates.

Electrical testing and full J2534 conformance remain outstanding. Firmware
updating, Wine and SocketCAN are out of scope.
