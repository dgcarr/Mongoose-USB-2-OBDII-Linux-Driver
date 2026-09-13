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
| CAN receive queue / ReadMsgs | Captured vendor frames | Yes | Host-queue path only; no bus traffic yet seen |
| CAN PASS filters | Captured vendor frames | Yes | Adapter accepts and acknowledges; filtering effect unproven |
| CAN transmit / WriteMsgs | Captured vendor frames | No | Windows reference only |
| ISO15765 | Captured host reassembly / firmware flow control | No | Windows reference only; timing variations pending |
| K-line / J1850PWM | Partial | No | Suitable hardware required |
| BLOCK filters / periodic / configuration | Partial | No | Pending |
| Programming-voltage output | Partial | No | Pending electrical validation |

All 14 core exports exist. CAN Connect/Disconnect, PASS filters and ReadMsgs are
implemented; other protocols remain unsupported. One CAN-family resource is reserved
per device, with a second CAN or ISO15765 connection refused locally. Transmit,
periodic and BLOCK-filter APIs reject invalid IDs and report ERR_NOT_SUPPORTED for
live channels. Unsupported IOCTLs and voltage output never report success. The Linux
driver has sent no bus messages or firmware writes; the Windows reference harness has
exercised OBD requests.

Read the receive and filter rows precisely. The adapter accepts our filter frames and
returns handles, and ReadMsgs drains a queue that the reader thread fills. Neither has
ever had a single bus frame to act on, so nothing here establishes that a filter
filters or that a received frame decodes correctly end to end.

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
synchronized competing lifecycle calls, plus pass-filter construction, filter handle
mapping, removal, receive decoding against 32 captured inbound frames, queue overflow
and malformed-frame rejection. Every scripted wire exchange must be consumed.

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

## Linux CAN pass-filter and receive acceptance (2026-09-13)

User confirmed the adapter was connected by USB only, with no car and no external 12 V.
`build/mongoose-client serial:SERIAL --can-receive-check` ran the production library
through three Connect/Disconnect cycles, each adding two pass filters, calling ReadMsgs,
and removing both filters. Twenty-six commands, every one status zero apart from the
documented `cJumpToFirmware` status 7 `Board already in firmware`.

- `cTableAddEntry` (`0x0d`) went out exactly as `can_pass_filter()` builds it: table
  selector 0, the channel's flag word at +16, type 1, pattern size 4, then mask and
  pattern with big-endian CAN IDs. Both the wildcard filter and mask `0x7ff` /
  pattern `0x7e8` were accepted on all three channels, including the `CAN_29BIT_ID`
  channel, where the filter body carried flags `0x100` to match.
- The add response is exactly 24 bytes, with the firmware handle at +20. That is the
  minimum our parser accepts, so the `response.size() < 24` guard sits precisely on the
  real boundary rather than above it.
- Six handles were issued: `0x0f5c`, `0x0fc4`, `0x1130`, `0x1198`, `0x1304`, `0x136c`.
  All six are distinct and ascending, and the two within a single channel are `0x68`
  apart. Six samples support nothing beyond that -- in particular they do not establish
  that handles always ascend, and a later 512-handle sample shows they do not. The
  handle is opaque, which is how the code treats it.
- `cTableRemoveEntry` (`0x0e`) carried table selector 0 and the handle, and was accepted
  for every filter. The vendor's captured example used selector 2 for a flow-control
  filter; both selectors work with the same body layout.
- **ReadMsgs emitted no wire traffic at all.** The 25 ms gap between the last add and the
  first remove is the read timeout elapsing against an empty host queue. Reads are served
  from the queue the reader thread fills; they do not poll the device.

Evidence: `analysis/captures/linux-can-receive-20260913T112728Z.trace`, with matching
`.strace`, `.log` and `.txt` metadata. These are tty syscall bytes captured with strace,
not usbmon packets. No bus traffic existed, so no filter was ever asked to pass or block
a frame, and ReadMsgs never returned a message. This is filter-acceptance evidence, not
filter-behaviour evidence.

## Linux filter-table probe (2026-09-13)

USB only, no vehicle and no external 12 V. `build/mongoose-diag --filter-probe` is a
bench mode: table and read operations on an open CAN channel, nothing transmitted, no
value written, no destructive opcode reachable. It reuses the shipping
`can_pass_filter()` encoder so it exercises the bytes the library actually sends.
Reproduced across two consecutive runs. Full detail in `PROTOCOL.md` section 7c.

- **512 pass filters accepted on one channel, no refusal**, against the at-least-40 the
  Windows capture established. 512 is the probe's ceiling, not the firmware's. The fill
  deliberately stopped short of allocator exhaustion, so the maximum remains unmeasured
  by choice.
- **`cTableClear` (`0x10`) returns status zero**, and a following remove of a
  previously-valid handle returns `0x0200` `FilterDelete: No matching Filter ID`. The
  second half is what makes the clear verifiable. `0x0200` is not in
  `analysis/ENUMS.md`, which starts the band at `0x0201`.
- **Handles are heap addresses on a 52-byte grid**, all 512 distinct, spanning `0x01c0`
  to `0x9190`, and not monotonic -- freed slots are reused. This corrects the reading
  taken from six handles in the receive check above.
- **`cGetValue` selector `0x2f` returns 1**, matching Windows. Value confirmed on a
  second platform; meaning still unknown.

### A driver ceiling this probe exposed

The first attempt failed at 250 filters with `all sequence numbers are in the 10-second
reuse quarantine`. That is our limit, not the adapter's. `Session` holds each of 255
sequence slots for ten seconds before reuse, so the library sustains roughly 25 commands
per second and no more; bursts shorter than the quarantine are unaffected, which is why
nothing before this had noticed. The probe now waits it out.

This is fine for channel and filter setup. It is not fine for transmit: the Windows
baseline moved 2455 msg/s, and every message needs a sequence. Either the quarantine
shortens on evidence about how long a late duplicate response can actually arrive, or
transmit needs a path that does not consume one slot per message. That question should
be settled before transmit is built, not after.

## Next blocking work

Pass filters and the receive queue are implemented and hardware-accepted, which
retires the first two items this list used to carry. What is left splits cleanly into
work the bench can finish and work that needs a vehicle.

Bench-reachable, with the adapter on USB alone:

1. Resolve the sequence-number ceiling described above. It bounds transmit throughput
   at roughly 25 messages per second, so it precedes transmit rather than following it.
2. Add CAN transmit. The `cOutboundData` body is settled by the static builder and a
   real capture together, including status `0x100` as success. With no bus, the bench
   can prove the adapter accepts and queues a frame and nothing more.
3. Measure receive throughput and back-pressure through a pty-backed synthetic load.
   That exercises the real tty reader, n_tty flip buffer, decoder and queue, but not
   the cdc_acm URB path, so it bounds host-side capability rather than proving parity
   with the adapter. Windows D4's five-minute ~2455 msg/s baseline is the reference.
4. Build the ISO15765 host-side reassembler against the captured VIN exchange. The
   responsibility split is known: firmware generates flow control, the host reassembles.

Vehicle-blocked:

5. Prove filters actually filter, transmit actually transmits, and received frames
   decode end to end. None of that can be shown without bus traffic.
6. ISO15765 timing — STmin, block size and N_Bs remain untested and unvaried.
7. Complete remaining protocol engines, periodic messages and IOCTLs with suitable
   vehicles/fixtures. C3/C4 are settled for this adapter: only one CAN-family
   channel can be open; further chan-field semantics cannot be inferred here.
8. Run 100 hardware cycles and a one-hour diagnostic soak once bus support exists.
   Three USB-only channel cycles and synthetic lifecycle tests do not satisfy these gates.

Electrical testing and full J2534 conformance remain outstanding. Firmware
updating, Wine and SocketCAN are out of scope.
