# Implementation and validation status

Bench work 2026-09-13 Australia/Sydney (captures use 2026-09-12 UTC); live-vehicle work 2026-09-19.
This is an experimental library: device management, raw CAN and 11-bit ISO15765 channels validated
on a live vehicle, **not a complete J2534 driver**. The 04.04 version string identifies the target
interface, not a compliance claim. The ordered list of what remains is `plan.md`.

The sections run in the order the work was done. Those dated 2026-09-13 are **bench** results with
the adapter on USB alone and no vehicle; where a later live-vehicle section supersedes a bench
statement, the bench section says so.

## Hardware results (bench, 2026-09-13)

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
- **Measured.** A pty-backed harness drives the real tty reader, line discipline,
  decoder and queue at 311500 msg/s with zero loss (see the load-harness section below),
  so the host side is not the constraint. `cdc_acm` itself under sustained inbound
  traffic was then measured on a live vehicle on 2026-09-19: 2448 msg/s for five minutes
  and 2456 msg/s for an hour, matching the Windows figure, with no overflow (see "Sustained
  receive on a live vehicle" and "One-hour diagnostic soak"). The libusb path used two
  outstanding 8 KB URBs while the tty path goes through the n_tty flip buffer, which
  throttles rather than drops when the reader lags; that difference was never a problem
  at vehicle rates. Zero loss is inferred from timestamp gaps, not independently counted.
- **Now exercised:** the partial-write retry loop. No read-only opcode produces a frame
  near the 0x1800 limit, so hardware alone could not reach it; the pty harness fills the
  buffer and the loop reports `partial tty write; command delivery is ambiguous` on the
  third maximum-size frame. A single 6144-byte write still completes in one call on an
  idle buffer.
- The two backends do not mutually exclude each other; see README.

## Capability matrix

| Feature | Static evidence | Implemented | Hardware validation |
|---|---|---|---|
| USB ownership, framing, startup/cleanup | Yes | Yes | USB-only success |
| Open/Close | Yes | Yes | USB-only success |
| ReadVersion | Yes | Yes | Firmware value retrieved |
| READ_VBATT / READ_PROG_VOLTAGE | Yes | Yes | Raw values retrieved; accuracy pending |
| ABI and GetLastError | Standard interface | Yes | Native client / offline tests |
| CAN Connect/Disconnect | Captured vendor frames | Yes | Bench success; live vehicle: ISO15765 open/close 100 cycles, raw CAN across every live run |
| CAN receive queue / ReadMsgs | Captured vendor frames | Yes | Live Volvo bus: ~2100 frames/s decoded end to end (2026-09-19) |
| CAN PASS filters | Captured vendor frames | Yes | Live bus: wildcard passes all, `0x7E8`/`0x7F8` mask passes none of the non-matching IDs |
| CAN transmit / WriteMsgs | Captured vendor frames | Yes | Live bus: timed write confirmed by `iMsgTxDone`; raw frames need ISO-TP PCI by hand; two ECUs replied |
| ISO15765 receive reassembly | Captured host reassembly / firmware flow control | Yes | Reproduces the captured VIN exchange offline |
| ISO15765 channel | Captured open, flow-control filter, single-frame request | Yes, 11-bit, single-frame transmit | Live Volvo: VIN request answered, multi-frame reply reassembled (2026-09-19) |
| ISO15765 timing (BS, STMIN) | Vendor decompile | Yes, via `SET_CONFIG` | Live: STMIN 20 ms gives 89 ms over 4 gaps; BS 2 gives 51 ms |
| ISO15765 segmented transmit | Vendor write path (no host segmentation) | Yes, up to 4095 bytes | Live: a 9-byte UDS request answered by the ECU |
| ISO15765 29-bit / N_Bs and other timeouts varied | Partial | 29-bit no; timeouts settable | Not exercised |
| K-line / J1850PWM | Partial | No | Suitable hardware required |
| BLOCK filters | Vendor builder decompile | Yes (CAN) | Live bus: blocks one ECU, passes the rest, removes cleanly |
| Buffer and filter-table IOCTLs | Vendor senders 1000daf0/1000dbf0, bench `cTableClear` | Yes | Live bus: CLEAR_RX discards the queue, CLEAR_MSG_FILTERS stops the flow |
| `GET_CONFIG` / `SET_CONFIG` (CAN, ISO15765) | Vendor decompile, `PROTOCOL.md` 7e | Yes | Live: all 19 ISO IDs read back as J2534 defaults; BS/STMIN change the ECU's frame timing |
| `LOOPBACK` | Vendor `loopbackBuf`, 7e | Yes (host-side) | Live: frame returns with `RxStatus 1` |
| Periodic messages (CAN) | Vendor senders `1000d220`/`1000d3c0`/`1000d4d0` | Yes | Live: 1 s period, stops cleanly |
| Programming-voltage output | Partial | No | Pending electrical validation |

All 14 core exports exist. CAN Connect/Disconnect, PASS filters, ReadMsgs and
WriteMsgs are implemented; other protocols remain unsupported. One CAN-family resource
is reserved per device, with a second CAN or ISO15765 connection refused locally.
Periodic and BLOCK-filter APIs reject invalid IDs and report ERR_NOT_SUPPORTED for
live channels. Unsupported IOCTLs and voltage output never report success. The Linux
driver has sent no firmware writes. Since 2026-09-19 it has put read-only OBD-II requests on
a live bus (raw CAN and ISO15765), and the Windows reference harness had already done so.

Read the receive and filter rows against the section that measured them. Until 2026-09-19 the
bench had no bus frame for a filter to act on, so the filter and receive rows rested on
acceptance evidence alone. The live-vehicle sections now show a wildcard filter passing traffic,
a narrow one blocking it, and received frames decoding end to end. What they do not show is
independent proof of zero loss, or anything about BLOCK filters, `PASS_FILTER` on an ISO15765
channel, periodic messages, configuration, or 29-bit addressing, which remain unimplemented.

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

All 12 CTest entries (the earlier ten plus `script_syntax` and `config`) pass in the normal, ASan/UBSan,
ThreadSanitizer and no-libusb builds, re-run on 2026-09-19. Earlier Clang/ThreadSanitizer/fuzzer results below describe
the preceding baseline, not a fresh run of the current code.

The preceding baseline passed under GCC, Clang, ASan/UBSan and ThreadSanitizer.
The `MONGOOSE_FUZZ` libFuzzer target was run over the framing codec for roughly
6.2 million executions seeded from the captured frames, with no crash, leak or
sanitizer finding.

That target now covers every path that parses untrusted wire data: the framing decoder,
the CAN receive queue, the transmit encoder and ISO15765 reassembly. The reassembler is
the one that most needed it, since it carries partial state across frames and takes its
lengths and sequence numbers from the wire. Two runs seeded from the captured frames,
175379 and 545593 executions, produced no crash, leak, sanitizer finding or timeout.

A sequence number is released as soon as its response arrives, and held for ten seconds
only when it is abandoned with a response possibly still outstanding. A response timeout
or an ambiguous write requires reopening the session, because a late or partially
delivered frame could otherwise be matched to a later command. A command whose deadline
expires **before** anything is written does not: nothing reached the wire, so no response
can ever arrive for that sequence, it is freed at once, and the session stays usable. The write budget is rounded up to whole
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

### A driver ceiling this probe exposed, since resolved

The first attempt failed at 250 filters with `all sequence numbers are in the 10-second
reuse quarantine` -- our limit, not the adapter's. Every sequence was held for ten
seconds regardless of outcome, capping the library near 25 commands per second. Bursts
shorter than the quarantine were unaffected, which is why nothing before this had
noticed. See the next section for how it was settled.

## Linux CAN transmit probe (2026-09-13)

USB only: no vehicle, no external 12 V, and no CAN bus. One frame was sent -- OBD-II
mode 01 PID 00 to the functional address `0x7DF`, the standard read-only capability
query -- once, through `build/mongoose-diag --transmit-probe`.

- `cOutboundData` returned status `0x100`, reproducing the queued status from the
  Windows capture on our own first transmit.
- The response echoes `chan=1` at body+8, so the firmware carries that field on the data
  path rather than ignoring it.
- **No `iMsgTxDone` (`0x0106`) arrived within two seconds.**

The last point is why the probe was worth running. No node existed to acknowledge the
frame, so the transmission cannot have completed, and the command reported success
anyway. `0x100` means the adapter accepted the frame, not that it sent it. Only the
`0x0106` indication separates the two, and the library now counts those.

Nothing here shows that a frame reached a wire. That needs a bus.

**Correction, 2026-09-19.** The probe's frame was described as mode 01 PID 00 but the bytes it
sent were `000007df0902`, a mode 09 PID 02 request with no ISO-TP length byte, so it was not a
valid single frame. The queued-versus-confirmed conclusion above does not depend on the
payload. The probe now sends the framed mode 01 request the text always described
(`02 01 00`, padded to 8 bytes), and on the live car it returns `0x100` and then one
`iMsgTxDone`. `mongoose-client --can-transmit-check` had the same defect and is fixed the same
way.

Evidence: `analysis/captures/linux-transmit-probe-20260913T120454Z.*`.

## The sequence-number quarantine, settled (2026-09-13)

The ten-second hold was a conservative guess made when nothing was known about how late
a duplicate response might arrive. Reviewing what it actually defends, against evidence:

**It could not protect the case it was written for.** Every path that abandons a sequence
with a response possibly outstanding -- a response timeout, or a write that may have been
partially delivered -- also sets `failure_`, which poisons the session permanently. After
either, no later command can run at all, so there is no command for a late frame to be
mismatched against. The path that expires before writing puts nothing on the wire, so no
response can ever exist. That leaves only sequences whose response already arrived, which
is precisely the case where the adapter has finished and nothing is outstanding.

**The adapter answers exactly once.** Across 1041 consecutive commands in the filter
probe, 1041 responses came back: one per request, no duplicate, no unsolicited
command-shaped frame. Inbound bus data does not consume the space at all -- the Windows
D4 capture shows 17233 of 17243 device-to-host frames are `cInboundData` carrying
sequence 0 -- so even a busy bus applies no pressure here.

So a sequence is now released when its response is matched, and held for ten seconds only
when abandoned. The hold is unreachable in practice, because abandoning one poisons the
session; it stays as a backstop if that ever changes.

Measured on the adapter, with no other change:

| | Before | After |
|---|---|---|
| 1041-command filter probe | 40.3 s, with ~40 s of waiting | 0.271 s |
| Sustained command rate | ~25/s (hard cap) | 3843/s measured, 6385/s on reads |
| 20000-command read burst | impossible: fails at command 256 | 3.13 s, 0 mismatched responses |

The 20000-command burst reused each of the 255 values about 78 times, roughly 40 ms
apart, and every response echoed the selector of the command it answered. Both the burst
and the filter probe were also run against the live adapter under ThreadSanitizer with no
warnings.

What this costs: the reuse distance for a given sequence value is now 255 commands rather
than ten seconds -- about 40 ms at full rate. A duplicate response arriving later than
that could in principle match a reused value. Nothing in 21000 commands of hardware
evidence suggests the adapter produces one, and the real protection against a genuinely
late frame remains the session poisoning that any timeout already triggers.

Transmit is therefore bounded by round-trip latency (0.26 ms mean, so ~3800 frames/s
serialized) rather than by the sequence design. That is above the 2455 msg/s the Windows
stack sustained on receive.

Evidence: `analysis/captures/linux-sequence-burst-20260913T121643Z.*`.

## Transmit reports delivery, not acceptance (2026-09-13)

J2534 already had the right home for `iMsgTxDone`, so the open question answered itself.
A write with `Timeout` zero queues and returns; a write with a non-zero timeout blocks
until the messages are sent and reports how many. The adapter says exactly when that
happened, so:

- `timeout == 0` counts a message once the adapter acknowledges it. That is acceptance,
  and the API says so.
- `timeout != 0` waits for one `iMsgTxDone` per message and counts only what the adapter
  confirmed, returning `ERR_TIMEOUT` with the confirmed count if they do not all arrive.

The indication is 1:1 with transmission in the vendor captures: e1 sent one frame and
produced one `0x106`; e2 sent one frame and produced one `0x106` plus an `0x10e` for the
firmware's own flow-control frame, which is a different code and not counted. The
indication also **echoes the sequence of the `cOutboundData` it confirms** (`seq=11` in
e1), so it is correlated rather than anonymous. The implementation counts rather than
correlates, because commands are serialized and only one write is ever outstanding.

On the bench, all three channels behave as they should: the queued write reports success,
and the timed write reports `ERR_TIMEOUT` with `confirmed=0`. Nothing is acknowledged
because there is no bus, and the driver now says so instead of reporting success for a
frame that never left the controller.

What was still unproven: that the timed path reports success on a live bus. It does: both
read-only queries in the 2026-09-19 vehicle run returned status 0 with `confirmed=1` (see
"First Linux run on a live vehicle").

Evidence: `analysis/captures/linux-transmit-confirm-20260913T122804Z.*`.

## Linux receive under sustained load (2026-09-13)

`build/mongoose-load-tests` stands a pty pair in for the adapter: the slave goes to the
real `tty.cpp` transport, so the run drives the actual reader thread, line discipline,
framing decoder and `CanReceiver` queue, replaying the 32 real inbound frames from the
Windows D1 capture. It is registered as the `load` CTest entry.

**It does not exercise the cdc_acm URB path.** This bounds what the host can absorb; it
does not prove parity with the adapter, and it is not a substitute for a busy bus.

- **1000000 messages at 311500 msg/s, zero lost, no overflow.** Every message is checked
  against the fixture frame that produced it, so loss, reordering or corruption fails the
  run. That is roughly 127x the Windows D4 baseline of 2455 msg/s: whatever ultimately
  bounds Linux receive, it is not the tty reader, the decoder or the queue.
- **A stalled consumer gets `ERR_BUFFER_OVERFLOW`**, not a silently short batch, so loss
  is reported rather than hidden.
- **The partial-write retry loop runs for the first time**, closing the coverage gap
  above. With nothing draining the master, two maximum-size 6148-byte frames go out and
  the third is refused as ambiguous delivery.

Clean under ASan/UBSan and ThreadSanitizer, the latter with zero warnings while the
reader thread runs.

Evidence: `analysis/captures/linux-pty-load-20260913T123404Z.*`.

## ISO15765 receive reassembly (2026-09-13)

The responsibility split is settled by capture: the adapter generates flow control on its
own -- it reports the FC frame as already transmitted and the host never sends one --
while the DLL reassembles. Reassembly is therefore a pure function of the inbound frame
sequence, with no timing loop and no wire traffic, which is the whole reason it can be
finished and proven correct with no vehicle.

`src/isotp.cpp` follows the vendor's own dispatch rather than the ISO standard in the
abstract: `10021790` routes on the PCI type and on whether a segmented receive is already
in progress, `10020fc0` handles single frames, `10021070` first frames, and `10021280`
consecutive frames. The behaviour that matters:

- A sequence gap **discards the whole partial message** rather than stitching a hole into
  it -- the vendor logs `ISO15765 SequenceNum got %d expected %d, killing receive` and
  clears its state.
- A first frame arriving mid-assembly abandons the partial message and starts over. This applies
  per source CAN ID; see "Multi-ECU multi-frame replies" for why.
- A single frame arriving mid-assembly is delivered and **leaves the partial message
  intact**, so later consecutive frames still complete it. That looks wrong and is what
  the vendor does; it is reproduced deliberately and marked as such in the code.
- A first frame announcing fewer than 8 bytes is rejected: a message that fits in a
  single frame must not be segmented.
- Content beyond the announced length is trimmed, so a padded final frame cannot
  lengthen the message.

Tested against the captured mode 09 PID 02 VIN exchange, byte for byte: three
`cInboundData` frames with PCI `10 14`, `21`, `22` produce one `START_OF_MESSAGE`
indication and one 24-byte message -- four identifier bytes plus the 20 announced --
matching what the vendor API returned. Fixtures are the captured bytes, not invented
ones.

Two deliberate divergences, both recorded in the code. The vendor copies a single frame's
announced length without checking the frame carries it, which reads past a short frame;
we reject instead, because delivering the difference means handing the caller whatever
followed in memory. And extended addressing is absent: the vendor shifts every offset by
one byte for it, but no capture exercises that path, and untested reassembly is worse
than none.

`PassThruConnect(ISO15765, ...)` has since been wired up; see "ISO15765 channel on a live
vehicle" below. What stays untouched is the STmin, block size and N_Bs behaviour, which
the adapter's own flow control decides and nothing here varies.

## First Linux run on a live vehicle (2026-09-19)

Adapter on a connected car, ignition on (battery ~11.9 V by `cGetValue` selector 3 before
the run), one 500 kbit 11-bit CAN channel through the production library. The user
confirmed a car was connected and the ignition on; the make, model and engine state were
not stated, so nothing below is tied to a particular vehicle. Reproduce with
`build/mongoose-client serial:SERIAL --vehicle-listen|--vehicle-check`. Listening never
transmits. `--vehicle-check` sends exactly two read-only OBD-II queries to `0x7DF`
(mode 01 PID 00, mode 09 PID 02). The older `--can-*` checks also connect at 250 kbit,
which is wrong for a 500 kbit bus, so they were not used on the car.

- **Receive works end to end.** A wildcard pass filter delivered 8520 frames in a 4 s
  passive window and 9818 in the next, decoding through the filter table, reader thread,
  queue and `ReadMsgs` with `RxStatus` 0 and device timestamps. First time a bus frame
  has crossed the Linux stack.
- **A timed `WriteMsgs` reports success on a live bus.** Both queries returned status 0
  with `confirmed=1`, so an `iMsgTxDone` arrived for each. This is the case the bench
  could only show failing, as `ERR_TIMEOUT` with `confirmed=0`.
- **A narrow filter filters.** A `0x7E8`/`0x7F8` pass filter reduced the same window from
  hundreds of frames to none. The one stray frame in an earlier run was ID `0x010`, most
  likely in flight while the filter was swapped, and did not recur once the queue was
  drained.
- **No ECU answered, until the request was framed correctly.** Three runs (ignition on,
  then engine running, with the same result) drew nothing in `0x700`-`0x7FF`, although
  both frames were confirmed transmitted. The cause was ours: the Windows OBD requests go
  through an ISO15765 channel as just `07df 0902` with `ISO15765_FRAME_PAD`, and the
  adapter adds the ISO-TP length byte. Our raw CAN channel sent `09 02` as the whole CAN
  payload, which is not a valid ISO-TP frame, so the ECUs ignored it. Sending the single
  frame by hand as `02 09 02` padded to 8 bytes (`07df` + `020902555555555555`) fixed it.
- **Two ECUs answered mode 01 PID 00 on the Volvo (engine running).** `0x7E8` returned
  `06 41 00 98 3B A0 13` and `0x7E9` returned `06 41 00 88 18 00 13`: positive response
  `0x41`, PID 00, then each ECU's supported-PID bitmask. This is a request out and a reply
  in on a real vehicle, the first end-to-end diagnostic exchange on Linux.
- **The VIN request (mode 09 PID 02) drew no first frame.** A multi-frame reply needs a
  flow-control frame to `0x7E0` from the requester, which the adapter only generates for an
  ISO15765 channel. On a raw CAN channel that never happens, so no reply was seen. This is
  settled by the ISO15765 section below: the same request on an ISO15765 channel is answered.
- The bench `--transmit-probe` and `check_transmit` send the same un-framed `09 02`. They
  were harmless with no bus, but they would not draw a reply on a car.
- The 200-frame cap in the first draft of `--vehicle-check` filled before any reply could
  land, which is why the first run said nothing; the cap has been removed.

Evidence: `analysis/captures/linux-vehicle-check-20260919T061343Z.txt` (engine running,
correctly framed request, the two replies). `...T061130Z.txt` and `...T061306Z.txt` are the
un-framed runs, ignition on and engine running, that drew nothing. `...T061026Z.txt` and
`...T061104Z.txt` are earlier draft-tool runs with the narrow `0x7E8` filter and the
200-frame cap; they carry the stray-frame and zero-frame results
above. A first draft run, which showed the cap problem, was deleted as superseded.

## One-hour diagnostic soak on a live vehicle (2026-09-19)

`MONGOOSE_SOAK_SECONDS` unset, `build/mongoose-client serial:SERIAL --vehicle-hour`, same
Volvo, engine running for the whole hour, started 06:37:44 UTC. One raw CAN channel at 500
kbit with a wildcard pass filter draining the bus continuously, plus one read-only mode 01
request a second to `0x7DF`, rotating through PIDs 00, 05, 0C and 0D. Each reply had to
arrive within one second. The single-frame PCI byte is built by the tool.

| | Result |
|---|---|
| duration | 3600 s |
| frames received | 8842439 (2456/s, unchanged from the first minute to the last) |
| requests / replies | 3600 / 3600, none unanswered |
| write failures (`iMsgTxDone` not confirmed) | 0 |
| `ERR_BUFFER_OVERFLOW` | 0 |
| worst device-timestamp gap | 8400 us, 0 gaps over 10 ms |
| request-to-reply | 7.40 ms mean, 20.46 ms worst |
| resident memory | 4892 kB at start, 5344 kB from minute 10 to the end |

Memory rose by 452 kB in the first ten minutes and then did not move by a single kilobyte
over the next fifty, which is a buffer filling and not a leak. The frame rate matches the
Windows D4 figure (2455 msg/s) and the earlier five-minute Linux soak (2448/s), so the
result holds at the same load for twelve times as long.

What it does not show: the reply latency is bounded below by the tool's 5 ms read timeout,
so the mean is a ceiling on the true figure; nothing counts bus frames independently of the
adapter, so zero loss is inferred from gaps and the absence of overflow and not proven;
and it is one vehicle at one baud with a single channel open. It ran on the raw CAN path
and did not repeat the ISO15765 path for an hour. Evidence:
`analysis/captures/linux-vehicle-hour-20260919T063744Z.txt`.

## 100 hardware lifecycle cycles on a live vehicle (2026-09-19)

`build/mongoose-client serial:SERIAL --vehicle-cycles`, same Volvo, engine running. Each of
the 100 cycles is a full lifecycle in one process: `PassThruOpen`, `ReadVersion`, ISO15765
`Connect` at 500 kbit, a `0x7E8`/`0x7E0` flow-control filter, one read-only mode 01 PID 00
request to `0x7DF` that must draw a positive `0x41 00` reply from `0x7E8`, then
`StopMsgFilter`, `Disconnect` and `Close`. The run stops at the first failure.

**All 100 passed** with no failure at any step, in 6.0 s (about 60 ms per cycle). The
request-to-reply time, including the timed write's `iMsgTxDone`, was 5.72 ms mean, 4.27 ms
best and 14.49 ms worst. Every cycle got a reply, so the filter, transmit, flow-control and
receive paths worked on each fresh channel, not just the first.

Read this precisely. All 100 used the ISO15765 channel; the raw CAN path was exercised by
the earlier live runs but not cycled. It is one process reopening the device, so it does
not repeat the earlier 40-cycle bench figure of separate process starts. It says nothing
about durability over time: that is the one-hour soak, which is still to do. Evidence:
`analysis/captures/linux-vehicle-cycles-20260919T063522Z.txt`.

## Sustained receive on a live vehicle (2026-09-19)

`build/mongoose-client serial:SERIAL --vehicle-soak`: five minutes, passive, one wildcard
pass filter on a 500 kbit channel, same Volvo with the engine running, nothing transmitted.
This is the `cdc_acm` URB path under real load, which the pty harness could not reach.

| | Windows D4 | Linux `cdc_acm` |
|---|---|---|
| frames | 736512 | 734506 |
| rate | 2455 msg/s | 2448 msg/s |
| max device-timestamp gap | 6000 us | 6000 us |
| gaps over 10 ms | 0 | 0 |
| overflows / empty rounds | none reported | 0 / 0 |

The rates and gap profile agree to within a third of a percent, and no read reported
`ERR_BUFFER_OVERFLOW`. As with the Windows run, this does **not** prove zero loss: nothing
counts the frames the bus carried independently of the adapter, and the two runs saw
different traffic. What it does show is that the host, tty and `cdc_acm` keep up with the
rate the Windows stack sustained, with no back-pressure and the same worst-case gap.
Evidence: `analysis/captures/linux-vehicle-soak-20260919T062439Z.txt`.

## ISO15765 channel on a live vehicle (2026-09-19)

`PassThruConnect(ISO15765)` is now implemented, on the same Volvo, engine running. The
wire forms all come from Windows captures D3 and E2, and `tests/isotp_tests.cpp` pins the
two encoders to the vendor's bytes.

- **Open** is `cOpenChannel` then `cSetPin` to node `0x0601`, the same bodies as CAN.
- **`FLOW_CONTROL_FILTER`** is `cTableAddEntry` with table selector 2, body
  `02000000 40000000 00 <response ID> 00 <request ID> 00`, and is removed with selector 2.
  The adapter takes no mask, so a mask that does not cover the 11-bit ID is refused instead
  of being silently widened. The vendor's own step passed `0000ffff` and that is accepted.
- **Transmit** is the CAN ID plus service bytes only; the adapter adds the PCI byte. Only a
  single frame (ID plus 1..7 bytes) is sent, since no capture shows a segmented transmit.
  A write with no flow-control filter is refused with `ERR_NO_FLOW_CONTROL`, as the J2534
  spec requires; the vendor's behaviour there is unobserved.
- **Receive** feeds the segments through `IsoTpReassembler` inside the channel's receiver.

Result: mode 09 PID 02 to `0x7DF` returned a `START_OF_MESSAGE` indication (`RxStatus 0x2`,
four ID bytes) and then one 24-byte message from `0x7E8`: the ID, `49 02 01`, and the
17-character VIN. The write was confirmed (`confirmed=1`), and the host sent no flow
control of its own. This reproduces on Linux exactly what Windows capture E2 shows.

The VIN is redacted in the committed evidence with `analysis/redact_vin.py`, as the earlier
captures were. Evidence: `analysis/captures/linux-vehicle-iso-20260919T062314Z.txt`.

Not covered: 29-bit ISO15765 (no capture of its filter layout, so refused with
`ERR_INVALID_FLAGS`), segmented transmit, extended addressing, `PASS_FILTER` on an ISO15765
channel, and any variation of STmin or block size.

## Linux script runner against the Windows reference (2026-09-19)

`mongoose-client [serial:SERIAL] --script FILE [--gap MS]` runs the same step files under
`tools/scripts/` that the Windows harness runs, with the same verbs, symbol tables and log lines,
so one experiment can be run on both stacks and compared. `mongoose-client --script-check FILE...`
parses without touching the adapter; the `script_syntax` CTest runs it over every script so the two
harnesses cannot drift apart. There is deliberately no step for `PassThruSetProgrammingVoltage`.

First live comparison, `e1-obd-mode01-pid00` (ISO15765, flow-control filter `0x7E8`/`0x7E0`, mode 01
PID 00 to `0x7DF`), same Volvo as the Windows capture. Every step returned the same result on both
stacks, and both received `000007e84100983ba013`. **One real difference, since fixed:** Windows also
delivered a message ahead of the reply, `RxStatus 9` (`TX_MSG_TYPE | TX_DONE`), TxFlags `0x40`, size
4, `ExtraDataIndex` 0, data `000007df` -- the transmitted request's CAN ID as J2534's transmit
indication for ISO15765. The first Linux run delivered only the reply; the same message is in
Windows capture E2.

The fix: `CanReceiver::note_transmit` records each ISO15765 request before it is sent, and the
adapter's `iMsgTxDone` (which carries only a timestamp) is paired with the oldest recorded request
in transmit order, queuing the message with the adapter's own timestamp. The start-of-message
indication now reports `ExtraDataIndex` 0 as Windows does, where it previously reported 4. Raw CAN
delivers no such message, and no capture shows otherwise. `isotp_tests` pins the message fields,
the pairing order, a refused write leaving nothing behind, an unrecorded confirmation, a
confirmation for another node, raw CAN, and `stop()`.

After the fix, `e1` and `e2` run on Linux against the same car and compared with the Windows logs
with timestamps and error text stripped (`e2` also with the VIN bytes) show no differing line. The
one remaining textual difference is the `ReadMsgs` error string, which is ours ("CAN read returned
fewer messages than requested" against Windows "Only read 2 of 16 messages"); the result code, 9, is
the same.

Evidence: `analysis/captures/linux-script-e1-20260919T075555Z.txt` (before the fix),
`analysis/captures/linux-script-e2-20260919T081049Z.txt` (after, VIN redacted), against
`analysis/captures/windows/20260913T161923-e1-obd-mode01-pid00/api.log` and
`.../20260913T161953-e2-obd-mode09-pid02-vin/api.log`.

## ISO15765 replies, silence and addressing on a live vehicle (2026-09-19)

Three scripts run with `mongoose-client --script` on the same Volvo, all read-only OBD-II (services
`01`/`09`) or a UDS `0x22` read, 500 kbit, one ISO15765 channel.

- **No reply is a partial read, not an error.** `e4` sent mode 01 PID `0x7F` (an unsupported PID) to
  `0x7DF`. No ECU answered in 3 s, and the read returned exactly one message, the transmit
  indication, with result 9 (`ERR_TIMEOUT`, partial). With nothing queued at all the read is
  `ERR_BUFFER_EMPTY`. Both are pinned in `isotp_tests`. There is no Windows capture of `e4`, so this
  is our own evidence of the adapter's behaviour, not a comparison.
- **Two flow-control filters coexist on one channel.** `0x7E8`/`0x7E0` and `0x7E9`/`0x7E1` were added
  together (handles 3 and 4) and both removed cleanly.
- **Physical addressing works for both ECUs.** Mode 01 PID 00 to `0x7E0` was answered from `0x7E8`
  and to `0x7E1` from `0x7E9`, with the same supported-PID masks the functional request drew. The VIN
  request to `0x7E0` came back as a start-of-message indication and one 24-byte message, reassembled.
- **Silence holds for physical requests too.** PID `0x7F` to `0x7E0` and to `0x7E1` drew no reply.
- **A request an ECU cannot serve draws a negative response** as an ordinary 7-byte message with
  `RxStatus 0`, needing no special handling: mode 09 with an unsupported info type (`09 FF`) got
  `7F 09 12` (sub-function not supported), and UDS `22 FF FF` got `7F 22 31` (request out of range),
  both from `0x7E8`.

Not covered: no ECU other than `0x7E8`/`0x7E9` was tried, and `ISO15765_FRAME_PAD` off. (Whether
the second ECU sends multi-frame replies is answered in the next section: it does.) The negative responses' CAN-level padding bytes were not
captured (the library reports only the reassembled message), so the test uses zeros for them.

Evidence: `analysis/captures/linux-script-e4-20260919T081209Z.txt`,
`linux-script-h1-20260919T081246Z.txt` (VIN redacted), `linux-script-h2-20260919T081316Z.txt`.
Scripts: `tools/scripts/h1-physical-addressing.txt`, `h2-negative-response.txt`.

## Multi-ECU multi-frame replies, and a reassembler bug they exposed (2026-09-19)

`h3-multiframe-reads` sends functional read-only requests with a flow-control filter for each ECU:
mode 09 info types 00, 04 (calibration IDs), 06 (calibration verification numbers) and 0A (ECU
name), then modes 03, 07 and 0A (stored, pending and permanent DTCs). Mode 04, which clears codes,
is never sent.

**The bug.** For PIDs 04 and 0A both ECUs began a multi-frame reply (two start-of-message
indications, `0x7E8` and `0x7E9`), but only one message completed each time. `IsoTpReassembler` held
one assembly, so the second ECU's first frame discarded the first ECU's partial message, which is the
vendor's rule for a first frame that interrupts a segmented receive. That rule is right within one
sender and wrong across senders: interleaved replies to a functional request are the ordinary OBD-II
case. No Windows capture has two ECUs sending multi-frame replies, so the vendor's own handling of
this is unobserved; Windows may well have the same defect.

**The fix.** One conversation per source CAN ID, at most 32 at a time (the oldest is given up when a
33rd starts, so unfinished conversations cannot grow memory or lock out a real reply). Within one ID
every vendor rule is unchanged: a first frame restarts that ID's message, a single frame leaves it
intact, a sequence gap kills it. A gap or restart on one ECU no longer touches another's.
`isotp_tests` reproduces the failure from the two captured messages (frames built from them by the
ISO 15765-2 split), and covers the restart, gap and limit cases. The existing reassembler tests pass
unchanged. Re-run on the car, `09 04` returns both ECUs' calibration IDs (39 and 23 bytes) and `09 0A`
returns both names (`ECM-EngineControl` from `0x7E8`, `TCM-TransmisCtrl` from `0x7E9`).

Also observed: PID 06 is a single frame from `0x7E9` and an 11-byte segmented message from `0x7E8`;
modes 03 and 07 return an empty list (`43 00`, `47 00`) from both ECUs, and mode 0A (permanent DTCs)
is answered by `0x7E9` alone. The car has no stored DTCs, so a multi-frame **DTC list** has not been
exercised; the calibration and ECU-name replies stand in as multi-frame evidence. Mode 02 (freeze
frame) was not run.

The reassembler was rebuilt under ASan/UBSan (11/11) and the libFuzzer target over it ran 17213
executions in 91 s with no crash, leak or finding; that is far fewer than the earlier 175k and 545k
runs and the reason is not known, so treat the fuzz result as a smoke test.

Evidence: `analysis/captures/linux-script-h3-20260919T081450Z-single-assembly.txt` (the bug) and
`...T081902Z-per-id-fix.txt` (fixed). Script: `tools/scripts/h3-multiframe-reads.txt`.

## ISO15765_FRAME_PAD on and off (2026-09-19)

`h4-no-padding` repeats a functional mode 01 PID 00 request and a physical VIN request through an
ISO15765 channel with `ISO15765_FRAME_PAD` off (flags `NONE` on the flow-control filter and the
writes). Both writes were accepted and confirmed transmitted (`iMsgTxDone`, the transmit-done
message shows TxFlags 0), and **no ECU answered either request**, though the same requests are
answered every time with padding on.

`h5-short-vs-padded-raw` separates the ECUs from the library. On a raw CAN channel, where the frame
goes out exactly as written, the same mode 01 PID 00 request (PCI 02, service 01, PID 00) to `0x7DF`
was sent three ways: three bytes (DLC 3), eight bytes padded with `0x00`, and eight bytes padded with
`0x55`. The three-byte frame drew no reply; **both eight-byte frames drew a reply from each ECU**,
`0x7E8` and `0x7E9`, with the usual supported-PID masks. So the silence in H4 is the Volvo's ECUs
ignoring a short request frame, not a library fault, and the padding byte value does not matter to them.

Consequence: on this car every request needs `ISO15765_FRAME_PAD`. The library passes the flag
through to the adapter and does not add padding itself. Every tool in this repository already sets it.

Not established: what byte value the adapter itself pads with (no independent view of the
transmitted frame), and whether the adapter pads a flow-control frame when the flag is off. A raw
capture of the request as it goes out would need a second CAN node or logger.

Evidence: `analysis/captures/linux-script-h4-20260919T082333Z.txt` and
`linux-script-h5-20260919T082400Z.txt` (filtered: ~9,500 unrelated bus frames removed, as its header
states). Scripts: `tools/scripts/h4-no-padding.txt`, `h5-short-vs-padded-raw.txt`.

## BLOCK filters on a live vehicle (2026-09-19)

`StartMsgFilter(BLOCK_FILTER)` is implemented on a raw CAN channel. The layout was the one open
question: the vendor builder `1000e600` (decompiled, `analysis/decompiled/senders/1000e600.c`)
selects table selector 0 with type byte 1 for a pass filter and **table selector 1 with type byte 2**
for a block filter, with the rest of the body identical. No Windows capture of a block filter exists
(`d3-block-filter` was never run), so the pairing came from the decompile alone until this run.
Each filter now remembers the wire table it was added to, and `StopMsgFilter` removes it from that
table, so a block filter is removed with selector 1.

On the car (`h6-block-filter-live`, raw CAN, engine state as in earlier runs): a pass-all filter, then
a BLOCK filter for `0x7E8`, then the padded functional mode 01 PID 00 request.

| Window | `0x7E8` frames | `0x7E9` frames | Distinct other IDs |
|---|---|---|---|
| pass-all only (before the request) | 0 | 0 | 69 |
| BLOCK `0x7E8` active, request sent | **0** | 1 | 70 |
| BLOCK removed, request sent again | 1 | 1 | 71 |

The adapter accepted the block filter and its removal, blocked exactly the one ID, and passed
everything else. The selector/type pairing is therefore confirmed on hardware, not only by the
decompile. `channel_tests` pins the wire bytes for add (`0100000000000204 000007ff 000007e8`) and for
removal from table 1, and that a removed block filter is then an invalid ID.

Not covered: BLOCK filters on an ISO15765 channel (the adapter's flow-control table is separate, and
J2534 defines no block filter there), combinations of several block filters, and the case of a block
filter with no pass filter (J2534 says nothing is received without a pass filter; not tested).

Evidence: `analysis/captures/linux-script-h6-*.txt` (filtered: bus frames outside `0x700`-`0x7FF`
removed, with the tally computed from the full log in its header). Script:
`tools/scripts/h6-block-filter-live.txt`.

## Buffer and filter-table IOCTLs on a live vehicle (2026-09-19)

`PassThruIoctl` now implements `CLEAR_RX_BUFFER`, `CLEAR_TX_BUFFER`, `CLEAR_MSG_FILTERS` and
`CLEAR_PERIODIC_MSGS` on a channel (the channel ID goes in the first argument, as J2534 says for
channel IOCTLs; `READ_VBATT` and `READ_PROG_VOLTAGE` still take a device).

- **Wire forms.** `cIoctl` (`0x11`) with a four-byte selector, 2 for TX and 3 for RX, as the vendor
  senders `1000daf0` and `1000dbf0` build it, each waiting for the response. `CLEAR_MSG_FILTERS` sends
  `cTableClear` (`0x10`) with the table selector, once for each table the channel actually filled (0
  pass, 1 block, 2 flow control), and forgets those handles; with no filters it sends nothing.
  `CLEAR_PERIODIC_MSGS` sends nothing because no periodic message can exist yet.
- **Host side.** `CLEAR_RX_BUFFER` also empties the host receive queue, any partly reassembled
  ISO15765 conversation and a pending overflow report (which described discarded data), after the
  adapter's response so that frames arriving later are kept. `CLEAR_TX_BUFFER` forgets the recorded
  requests that will never be confirmed, so a later transmit-done cannot pair with one of them.
  A firmware refusal is returned as an error and flushes nothing.
- **Tests.** `channel_tests` pins the wire bytes, that queued frames disappear and later ones are kept,
  the refusal path, that cleared filters' handles become invalid, and that clears with nothing to clear
  send nothing.

Live (`h7-clear-buffers-live`, raw CAN, pass-all on the ~2450 frame/s bus): after a second of queuing
the oldest frame was at device time 3500 us; after another second and `CLEAR_RX_BUFFER` the next frame
was at 2004100 us, 2.0006 s later, so the stale queue was discarded. `CLEAR_TX_BUFFER` and
`CLEAR_MSG_FILTERS` returned status 0; after the filter clear and a final `CLEAR_RX_BUFFER` a 500 ms
read returned nothing (`ERR_BUFFER_EMPTY`), so the table clear did remove the pass-all filter.

Not covered: `CLEAR_TX_BUFFER` with frames actually waiting to send (a transmit is confirmed within
milliseconds on this bus, so the queue is never observably non-empty), and `CLEAR_MSG_FILTERS` on an
ISO15765 channel (table 2 is the same wire form, not run live).

Evidence: `analysis/captures/linux-script-h7-*.txt`. Script: `tools/scripts/h7-clear-buffers-live.txt`.

## Configuration, loopback, periodic messages and segmented transmit (2026-09-19)

These four were the "needs the Windows machine" items. They did not: decompiling the vendor DLL with
Ghidra (`analysis/ExtractCallers.java`, `analysis/ExtractByName.java`, output under
`analysis/decompiled/{config,periodic,write,outbound2,filter}/`) supplied every wire layout and rule, and
the car then confirmed each one. The findings are in `PROTOCOL.md` section 7e.

**`GET_CONFIG` and `SET_CONFIG`** (`src/config.cpp`, `PassThruIoctl`), for CAN and ISO15765, with the
vendor's ID-to-selector map, ranges and error codes; `NON_VOLATILE_STORE_2..10` are refused, and two
electrical parameters (`DT_PULLUP_VALUE`, `DT_HALF_DUPLEX`) are readable but not settable. Read on the car
(`h8`, `h9`, one parameter per call): raw CAN `DATA_RATE` 500000, sample point 80, jump width 15, loopback
0; ISO15765 the same plus BS 0, STMIN 0, BS_TX and STMIN_TX 65535, WFT_MAX 0, N_BR_MIN 0, pad value 0, the
four N_* timeouts 1000, N_CS_MIN 0, pull-up 0, half duplex 0 -- all 19 IDs answered with J2534 defaults
through the map. An ID the channel does not have returns `ERR_NOT_SUPPORTED`; `J1962_PINS` returns
`ERR_FAILED`; an ISO-only ID on a CAN channel is `ERR_NOT_SUPPORTED`.

**ISO-TP timing.** `SET_CONFIG` of `ISO15765_BS` and `ISO15765_STMIN` reaches the adapter's own
flow-control frames and controls how the ECU sends a multi-frame reply. `h10` requests mode 09 PID 04 (a
35-byte reply, first frame plus five consecutive frames) from the engine ECU, five times per setting. Time
from first to last frame, from the start-of-message and completed-message timestamps:

| BS | STMIN | runs (ms) | mean |
|---|---|---|---|
| 0 | 0 | 15.9 9.2 9.3 9.7 11.6 | 11.1 |
| 0 | 20 | 89.0 89.2 89.7 89.3 89.7 | 89.4 |
| 2 | 0 | 15.6 15.2 9.3 9.7 9.7 | 11.9 |
| 2 | 20 | 49.7 55.6 53.5 49.5 48.7 | 51.4 |

STMIN 20 ms over the four gaps between five consecutive frames is 80 ms plus about 9 ms, and the runs
agree to 0.7 ms. With BS 2 the consecutive frames go in blocks of two, and STmin applies only between the
frames inside a block, so two gaps, 40 ms plus the flow-control round trips: 51 ms, faster than BS 0 with
the same STMIN, exactly as ISO 15765-2 says. Defaults were restored and read back after the run. N_As,
N_Ar, N_Bs, N_Cr and the other timeouts are settable but were not varied.

**`LOOPBACK`** is a host-side flag in the vendor too (never sent to the firmware); with it set, a
confirmed transmit is queued back as a received message with `RxStatus TX_MSG_TYPE` carrying the whole
frame and the adapter's timestamp, after the transmit-done message on ISO15765. On raw CAN (`g4`) with
LOOPBACK 0 the write was confirmed and a 500 ms read returned nothing; with LOOPBACK 1 the same write
returned `000007df0201005555555555` with `RxStatus 1` and timestamp 2108900; restored to 0.

**Periodic messages** (raw CAN only; ten per channel; interval 5..65535 ms): `cTableAddEntry` on table 4,
`cTableRemoveEntry` and `cTableClear` with selector 4, all from the vendor senders. `g1` on the car: a
read-only mode 01 PID 00 request every 1000 ms drew a reply from each ECU at 0.41, 1.41, 2.41, 3.42 and 4.42 s
(one period 1.000-1.010 s), and after `StopPeriodicMsg` a two-second read returned nothing. A channel with live
periodic messages has the table cleared before it is closed, so nothing can keep transmitting. A running
periodic message is confirmed by the adapter too, so only confirmations that pair with a recorded write now
count toward a timed `WriteMsgs`; `channel_tests` pins that an unrelated confirmation does not satisfy one.

**Segmented ISO15765 transmit.** The vendor does not segment: its write path (`1000c270`) and frame builder
(`1006b090`) copy the message as given, so the adapter segments it. The single-frame limit is lifted: an ID
plus 1..4095 bytes goes out in one `cOutboundData`. `e5` on the car sent the 9-byte UDS request
`22 F190 F187 F18C F194` to the engine ECU; the write was confirmed and the ECU answered with a positive
`0x62` response, a 42-byte multi-frame message our reassembler completed. Two of the four identifiers came
back, `F190` and `F18C`. `F18C` straddles the boundary between the two frames we sent, so the ECU could only
have answered it by reassembling them correctly. `F187` and `F194` were not in the reply; that fits an ECU
that does not support them (UDS ECUs omit unsupported identifiers), but that reading is an inference. The
payload, which carries the VIN and module identifiers, is masked in the saved evidence.

**Confirmations paired by sequence, and a transmit chan field.** The vendor tags every transmitted message
with the command's sequence and a count of messages still to send, and matches `iMsgTxDone` against both
(`1000bcb0`). The driver now records each request under its sequence before it is sent (`Session::command`
reports the sequence first, since the confirmation can beat the command response) and pairs by sequence,
discarding older unpaired records, instead of by arrival order. The data command's chan field is the
number of messages remaining in the call, 1 for a single write; `WriteMsgs` of several messages now sends 3, 2,
1. *(Superseded 2026-09-20: that count-down never worked on the adapter; each message is now a transaction of its
own. See "SocketCAN bridge, and a multi-message write bug it exposed".)* `channel_tests` covers a full ISO15765 exchange through the public API: filter, timed write, confirmation
ahead of the response, three reply frames, and the read returning the transmit-done, start-of-message and
reassembled messages in order.

**Two more vendor answers.** PASS and BLOCK filters on an ISO15765 channel are refused ("ISO15765 cannot
establish Pass/Block filters", error code 7), so this driver keeps refusing them; flow-control filters are
capped at 64 ("Only 64 filters are permitted total"), now enforced. The vendor also caps pass/block filters
at 10 when a device field equals 1; Windows capture D2 accepted at least 40, so that field is not 1 on this
unit and the sample-point limits gated by the same field (68..80 on CAN, 80 only on ISO15765) are looser in
the vendor than the ones enforced here.

**A driver bug this exposed.** An ISO15765 channel was closed on the CAN node (`0x0501`) instead of its own
(`0x0601`), because `Disconnect` reset the recorded protocol first. The adapter tolerated it, so the earlier
100-cycle run passed, but the vendor closes on `0x0601` (Windows capture E1) and it now does too. After the
fix both 100-cycle runs still pass (ISO15765: 5.05 ms mean request-to-reply; raw CAN: 5.04 ms).

**One hour on the ISO15765 path.** `--vehicle-hour-iso` (new): two flow-control filters, a functional mode 01
request every second (PIDs 00, 05, 0C, 0D) and mode 09 PID 04 every tenth second, whose two ECUs answer with
interleaved multi-frame messages. 3600 s: 3600 requests; **3240 of 3240 single-frame replies and 360 of 360
two-ECU multi-frame replies complete**; 0 write failures, read errors, overflows or stray messages; 3600
transmit-done messages and 720 start-of-message indications as expected; reply time 9.14 ms mean, 26.22 ms
worst at 5 ms read granularity; resident memory 4896 kB at start and 5108 kB at the end, unchanged for the
last 50 minutes. It ran on the build before the sequence pairing, configuration, periodic and loopback
changes above, so it covers per-ID reassembly and the transmit-done message but not that later code; the
two 100-cycle runs and the scripts above ran on it.

**Tests.** 12 CTest entries (the new `config` among them), including wire scenarios for configuration,
periodic messages and their limit, ISO15765 configuration, the flow-control limit and the ISO15765 exchange.
All 12 pass in the normal, ASan/UBSan, ThreadSanitizer (zero warnings) and no-libusb builds, run on the final tree. The libFuzzer target now also drives the periodic,
block-filter and ISO15765 transmit encoders, the configuration table and the sequence-paired receiver with
loopback: 4417 executions in 2 minutes, no crash, and that is a smoke test given how slowly it ran.

Evidence: `analysis/captures/linux-script-{h8,h9,h10,g4,g1,e5}-*.txt`, `linux-vehicle-hour-iso-*.txt`,
`linux-vehicle-cycles-{raw,iso-rerun}-*.txt`. Scripts: `tools/scripts/h8-getconfig-can.txt`,
`h9-getconfig-iso15765.txt`, `h10-stmin-bs-matrix.txt` and the `g1`, `g4`, `e5` scripts written for Windows,
which run unchanged through `mongoose-client --script`.

## Ignition off, bus sleep and wake (2026-09-19)

`mongoose-client --vehicle-ignition` (new) is a passive observer: one raw 500 kbit channel with a wildcard filter,
one log line a second (frame rate, bus active or quiet, `READ_VBATT`, read errors by code), and a read-only mode 01
PID 00 request every ten seconds, sent only while frames are arriving. You switched the ignition off, locked the
car, and later switched it on again with the channel open the whole time.

**Run 1** (`linux-vehicle-ignition-run1-*.txt`, 359 s). The ignition went off about 17 s in. Traffic stepped down
in stages, 2470 to 2100 to 1300 to about 700 frames/s over roughly 15 s, as modules shut down, and the engine ECU
stopped answering (31 of the 36 requests went unanswered). The bus **stayed active at about 700 frames/s for the
remaining six minutes**.

**Run 2** (`linux-vehicle-ignition-run2-*.txt`, 168 s), started about 6.5 min after the ignition went off. After two
unanswered requests the observer transmits nothing more. **The bus went quiet 32.8 s in**, 22 s after the last
request, and stayed quiet (0 frames/s). The ignition was switched on at about 68 s: **the bus woke at 67.9 s**, the
first request after the wake went unanswered (the engine ECU was still starting), every later one was answered in
20.1 ms, and traffic returned to about 2470 frames/s. The channel was never reopened.

What the driver did, over both runs (about 9 minutes, 300000+ frames): no read error, overflow or write failure of
any kind; the same channel and filter delivered frames before the sleep and after the wake; device timestamps kept
counting; `ReadMsgs` returned the ordinary empty result through the quiet spell. No `eVbattLoss` (`0x020a`) indication
appeared, which is expected: the adapter kept its 12 V through the OBD connector throughout, so nothing here
exercises the vendor's voltage-loss path.

Read the two claims that go beyond that carefully. (1) Whether the periodic requests kept the bus awake in run 1 is
**not established**: the bus went quiet about 7 min after the ignition went off, and run 1 stopped at 6 min, so
the two runs are equally consistent with a sleep timer of about seven minutes and with requests delaying sleep. A
run that is silent from the start would separate them. What run 2 does show is that listening alone does not
prevent sleep or wake. (2) Battery voltage read 12.6 V with the ignition on and the engine off, rose to 13.0-13.3 V
after the ignition went off, and settled at 12.7-12.8 V after the wake. A rise on ignition-off is unusual;
something charging the battery would explain it, but the cause was not determined.

Not tested: pulling the USB cable with traffic running, and the adapter losing vehicle power while USB stays
connected (the `eVbattLoss` path).

## Next blocking work

The ordered plan is `plan.md`; this section only records what the validation evidence leaves open.
Everything the bench could settle is done, and the live-vehicle runs of 2026-09-19 closed most of the
vehicle items. Done on the live Volvo:

1. ~~Prove filters filter, transmit transmits and received frames decode end to end.~~
2. ~~Confirm that a timed `WriteMsgs` reports success on a live bus.~~
3. ~~`cdc_acm` throughput parity.~~
4. ~~100 hardware cycles and a one-hour diagnostic soak.~~ Both done on raw CAN and on ISO15765.
5. ~~ISO15765 timing (BS, STMIN), segmented transmit.~~ Measured and working.
6. ~~TX-done messages, `LOOPBACK`, periodic messages, `GET_CONFIG`/`SET_CONFIG`, BLOCK filters,
   the buffer IOCTLs.~~ Implemented and checked on the car. Raw CAN delivers no transmit-done message
   without `LOOPBACK`, by the vendor's own logic (`PROTOCOL.md` 7e).
7. `PASS_FILTER` on an ISO15765 channel is refused by the vendor and by this driver, by design.

Still open, and needing something this project does not have or has decided not to do:

- **Other protocols:** K-line (ISO9141/14230, `FIVE_BAUD_INIT`, `FAST_INIT`), J1850 VPW/PWM and
  the pin-switched `*_PS` protocols need a vehicle or bench ECU on those buses. There is no wire
  evidence for any of them beyond the vendor's static code.
- **29-bit ISO15765** and J1939: no capture of the filter layout, and this car has no 29-bit ECU.
- **Programming voltage output:** electrical, not to be tried on a vehicle; `SetProgrammingVoltage`
  returns `ERR_NOT_SUPPORTED`.
- **Not exercised on the car:** unplugging USB under load and losing vehicle power under USB (the ignition-off and wake cycle is done, see above), `DATA_RATE` and the sample point/jump width settings (they change the
  bus timing), the ISO15765 timeouts N_As/N_Ar/N_Bs/N_Cr (settable, never varied), `DT_PULLUP_VALUE` and
  `DT_HALF_DUPLEX` (readable only here), more than one periodic message on the wire, `CLEAR_TX_BUFFER`
  with frames waiting, and the flow-control table filled to its 64 limit.
- **Independent proof of zero receive loss:** nothing counts the frames the bus carried apart from the
  adapter. A second CAN logger would.
- Live-traffic regression of the rewritten write and periodic paths, when the vehicle is next awake (the conformance
  pass and packaging are done; see the deployment readiness section below).
- The filter-table maximum on the adapter (stopping short of allocator exhaustion) and extended-address
  ISO15765 reassembly, which no capture exercises.

Firmware updating and Wine are out of scope. SocketCAN is reached through the userspace bridge
`mongoose-socketcan` (2026-09-20), not a kernel driver.

## Deployment readiness pass (2026-09-19)

What was checked, and what it does and does not prove.

- **J2534 conformance review.** Four read-only reviewers (lifecycle, messages, filters and periodic, ioctl and config)
  compared the 14 exports with the specification, as the reviewers recalled it, and with the vendor decompile; each
  finding was then checked against the code before anything was changed. Fixed and pinned by the `conformance` CTest
  (which fails on the old code): WriteMsgs partial counts and per-call confirmation counting, one uncapped deadline,
  outbound status `0x101` as `ERR_BUFFER_FULL`, periodic teardown after an ambiguous add, a NULL flow-control
  message. Deviations reviewed and kept are in `docs/USING.md`. The specification text itself was not available, so
  "conformant" here means agreement with the vendor DLL and the reviewers' recollection, not certification.
- **Bit-rate list, from Ghidra.** The vendor's baud predicate (`10037890`) accepts 18 standard CAN rates, or only three
  when a device field is 1. Its only writer is the device constructor and `PassThruOpen` passes 0, so the long list
  applies; Connect and `DATA_RATE` now enforce it (`PROTOCOL.md` 7e). Not exercised with an unlisted rate on the car.
- **Bug-check review** of the session, codec, tty, usb and tool layers found no defect in session and codec and fixed
  the real ones elsewhere: a long script comment could run as a command, the ignition observer could transmit before
  seeing traffic, the `--gap` value could silently become zero, and smaller tty, usb and tool error paths. Two
  script-runner defects have CTests that fail on the old code. Not fixed, on purpose: a tty device-swap race between
  discovery and open, and the libusb leak after a failed drain (deliberate, to avoid a use-after-free).
- **Fuzzing.** The codec, receiver (both protocols, with per-call transmit counting), reassembler and config fuzz target
  ran 1508 s on 10 workers, about 4.3 million executions, coverage 1075, no crash, timeout or out-of-memory. This
  finds crashes, not wrong answers. The target had also stopped compiling under clang after an earlier signature change
  (CI caught it); it builds and runs again.
- **Sanitizers.** All 15 tests pass under ASan/UBSan and ThreadSanitizer (with the GCC 13 suppression file) locally,
  and CI was green on the pushes that carried these changes.
- **On the adapter.** With the car's bus silent (ignition off), open, version, connect, filter and disconnect worked at
  500 kbit, 250 kbit and 29-bit, and a timed write reported that nothing was confirmed rather than claiming success.
  The write and periodic rewrites have **not** been run against live vehicle traffic this session.

## SocketCAN bridge, and a multi-message write bug it exposed (2026-09-20)

Adapter on USB, **no vehicle** (the bus is silent, so no frame is acknowledged). Interface `vcan0`, created once
with root. Evidence: `analysis/captures/linux-socketcan-bench-20260919T212830Z.txt`; probe `analysis/probes/write_batch_probe.c`.

- **The bridge, end to end, offline.** `socketcan_bridge` runs `mongoose-socketcan` built against a fake J2534 library
  (`tests/fake_j2534.c`) over a real `vcan`: 11-bit, 29-bit and empty frames arrive intact, a request written to the
  interface reaches the fake and its reply comes back, remote frames are refused, the listen-only default sends
  nothing, and a UDS VIN read through a kernel `CAN_ISOTP` socket completes. That last one is a multi-frame reply,
  so the kernel's flow-control frame has to go out through the bridge and the consecutive frames come back.
  `volvo_guide` runs the two Python examples of `docs/VOLVO.md` verbatim the same way (standard library, and
  udsoncan when installed). Both pass under ASan/UBSan and ThreadSanitizer, and five runs in a row were clean.
- **The bridge on the adapter.** Listen-only and `--transmit` start, run and stop on SIGTERM with exit 0; a
  second instance is refused with the device busy; an unlisted bit rate, a missing interface and a non-CAN
  interface are refused with a clear message. With no bus to acknowledge them, 200 frames written at once filled the
  adapter's transmit queue at 199; the 200th came back `ERR_BUFFER_FULL`, was counted as refused, and the bridge
  carried on.
- **The bug.** Before the fix, three frames written to the bridge at once killed it. Any `PassThruWriteMsgs` of more
  than one message timed out on its first data command, and the device then needed a reopen. The probe shows it
  directly: three one-message writes back to back were accepted in 1 ms, and one three-message call timed out after
  1000 ms. The command's chan field counted down the messages still to send (3, 2, 1), copied from the vendor's
  `1000c270`. But the vendor sends a whole call as **one transaction** before waiting, and the firmware answers
  once, after the message tagged 1. The driver waited for an answer after every command, so the first one was never
  answered. Each message is now a one-message transaction (chan 1, the only form any capture or car test had used).
  After the fix the probe's three-message call is accepted in 1 ms. `channel_tests` pins chan 1 for every message
  of a batched write.
- **Not covered.** The bridge has **not** been run on the car: not receive at the car's ~2450 frames/s, not a
  flow-control round trip against a real ECU, not bus-off or unplug while running. Whether an 11-bit channel with an
  all-pass filter also delivers 29-bit frames is unknown. The vendor's pipelined multi-message transaction was not
  implemented; it would save USB round trips but cannot be checked without a bus that acknowledges frames.

