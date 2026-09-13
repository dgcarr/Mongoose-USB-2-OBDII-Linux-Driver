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
- **Partly measured now.** A pty-backed harness drives the real tty reader, line
  discipline, decoder and queue at 311500 msg/s with zero loss (see the load-harness
  section below), so the host side is not the constraint. What remains unmeasured is
  `cdc_acm` itself under sustained inbound traffic: the libusb path used two outstanding
  8 KB URBs, while the tty path goes through the n_tty flip buffer, which throttles
  rather than drops when the reader lags. Vehicle traffic is where that would first
  matter, so parity is still not claimed.
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
| CAN Connect/Disconnect | Captured vendor frames | Yes | Linux USB-only success; no vehicle/12 V |
| CAN receive queue / ReadMsgs | Captured vendor frames | Yes | Host-queue path only; no bus traffic yet seen |
| CAN PASS filters | Captured vendor frames | Yes | Adapter accepts and acknowledges; filtering effect unproven |
| CAN transmit / WriteMsgs | Captured vendor frames | Yes | Queues on the bench; a timed write correctly reports nothing sent |
| ISO15765 receive reassembly | Captured host reassembly / firmware flow control | Yes | Reproduces the captured VIN exchange offline |
| ISO15765 channel / timing | Captured host reassembly / firmware flow control | No | Windows reference only; timing variations pending |
| K-line / J1850PWM | Partial | No | Suitable hardware required |
| BLOCK filters / periodic / configuration | Partial | No | Pending |
| Programming-voltage output | Partial | No | Pending electrical validation |

All 14 core exports exist. CAN Connect/Disconnect, PASS filters, ReadMsgs and
WriteMsgs are implemented; other protocols remain unsupported. One CAN-family resource
is reserved per device, with a second CAN or ISO15765 connection refused locally.
Periodic and BLOCK-filter APIs reject invalid IDs and report ERR_NOT_SUPPORTED for
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

What is still unproven: that the timed path reports success on a live bus. The Windows
evidence says it should.

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
- A first frame arriving mid-assembly abandons the partial message and starts over.
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

`PassThruConnect(ISO15765, ...)` still returns `ERR_NOT_SUPPORTED`. The reassembler is
the part that can be proven offline; wiring up the channel means STmin, block size and
N_Bs, which nothing on the bench can exercise.

## Next blocking work

Every item this list carried that the bench could reach is now done: pass filters and the
receive queue, the filter-table probe, CAN transmit and its delivery reporting, the
sequence-number ceiling, sustained receive with back-pressure, and ISO15765 reassembly.
What is left needs a vehicle, and saying so is the point of this section -- none of it can
be closed by more work on a desk.

Vehicle-blocked:

1. Prove filters actually filter, transmit actually transmits, and received frames decode
   end to end. Everything above establishes that the adapter accepts our frames and that
   the host handles what comes back; none of it shows a message crossing a bus.
2. Confirm that a timed `WriteMsgs` reports success on a live bus. The Windows captures
   show one `iMsgTxDone` per transmit, so it should; on the bench it correctly reports
   that nothing was sent.
3. ISO15765 timing -- STmin, block size and N_Bs remain untested and unvaried -- and
   wiring the reassembler to a live ISO15765 channel, which those parameters gate.
4. `cdc_acm` throughput parity. The pty harness bounds the host side at 311500 msg/s, but
   the URB path between adapter and kernel is untested under load.
5. Complete remaining protocol engines, periodic messages and IOCTLs with suitable
   vehicles/fixtures. C3/C4 are settled for this adapter: only one CAN-family channel can
   be open; further chan-field semantics cannot be inferred here.
6. Run 100 hardware cycles and a one-hour diagnostic soak once bus support exists.
   Three USB-only channel cycles and synthetic lifecycle tests do not satisfy these gates.

Bench work that remains possible but was deliberately not done: the filter-table maximum
(stopping short of allocator exhaustion), the channel IOCTLs for clearing buffers, and
extended-address ISO15765 reassembly, which no capture exercises.

Electrical testing and full J2534 conformance remain outstanding. Firmware
updating, Wine and SocketCAN are out of scope.
