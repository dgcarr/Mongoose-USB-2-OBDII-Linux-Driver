# Implementation and validation status

2026-09-13 Australia/Sydney (captures use 2026-09-12 UTC).
This is an experimental device-management library, **not a complete J2534 driver**.
The 04.04 version string identifies the target interface, not a compliance claim.

## Hardware results

Adapter 18e1:0104, serial AOLHE0000003666A, USB only; user confirmed no vehicle attached.

- Vendor control 0xdb value 1 succeeded; Echo and GetBoardInfo returned valid frames.
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

Evidence: `analysis/captures/linux-*.trace`. These are timestamped libusb application
transfer logs, **not usbmon pcaps**. Original traces are retained. Replay fixtures
strip only timestamps and control records; no successful response was synthesized.
The earlier `linux-inspect-...trace` contains only open/close, before the inspection
mode was corrected; `linux-values-...trace` is the actual value-query capture.

## Capability matrix

| Feature | Static evidence | Implemented | Hardware validation |
|---|---|---|---|
| USB ownership, framing, startup/cleanup | Yes | Yes | USB-only success |
| Open/Close | Yes | Yes | USB-only success |
| ReadVersion | Yes | Yes | Firmware value retrieved |
| READ_VBATT / READ_PROG_VOLTAGE | Yes | Yes | Raw values retrieved; accuracy pending |
| ABI and GetLastError | Standard interface | Yes | Native client / offline tests |
| CAN channels and message I/O | Partial | No | Requires reference capture and vehicle |
| ISO15765 | Partial host-side logic | No | Requires reference capture and vehicle |
| K-line / J1850PWM | Partial | No | Suitable hardware required |
| Filters / periodic / configuration | Partial | No | Pending |
| Programming-voltage output | Partial | No | Pending electrical validation |

All 14 core exports exist. Connect explicitly returns ERR_NOT_SUPPORTED; no channel
IDs are allocated. Channel APIs reject invalid IDs. Unsupported IOCTLs and voltage
output never report success. No bus messages or firmware writes have been sent.

## Tests

CTest covers frame fragmentation, concatenation, invalid headers, bounds, randomized
round trips, general-response matching, unrelated indications, short responses,
sequence quarantine, timeout poisoning, cancellation, unplug notification, short
writes, pre-write deadline expiry, transport root-cause preservation, concurrent
command serialization, replay mismatches, C ABI layout, symbol exports, and 100
**synthetic** lifecycle cycles. Real discovery/open/inspection captures are replayed
separately through the diagnostic executable.

The suite passes under GCC, Clang, and ASan/UBSan, and is clean under ThreadSanitizer.
The `MONGOOSE_FUZZ` libFuzzer target has been run over the framing codec for roughly
6.2 million executions seeded from the captured frames, with no crash, leak or
sanitizer finding; that exercises the only code that parses untrusted wire data.

Sequence numbers are held for ten seconds before reuse. A response timeout or an
ambiguous write requires reopening the session, because a late or partially delivered
frame could otherwise be matched to a later command. A command whose deadline expires
**before** anything is written does not: nothing reached the wire, so only the sequence
slot is lost and the session stays usable. Hardware bounds on delayed duplicate
responses are still unknown. No firmware-generation marker has been established.

## Next blocking work

1. Run the Windows capture harness on the user's laptop, using this repository's
   vendor installer/DLL. Capture metadata, Open/ReadVersion/Close, then controlled
   CAN Connect/filter/read cases.
2. Trace channel/resource allocation and pin routing against those captures before
   enabling Connect. Exports in `analysis/decompiled/linux_bringup/` extend the
   evidence but do not settle constructor signatures.
3. Validate CAN/ISO15765 on the user's **2017 Volvo XC60 D5 AWD**. It is not currently
   connected; no ignition state, baud rate, ECU addressing, or pin assumptions are made.
4. Complete the remaining protocol engines, filter/periodic semantics and IOCTLs.
   Obtain other vehicles/fixtures for protocols the Volvo cannot exercise.
5. Run the planned 100 hardware cycles and one-hour diagnostic soak after bus support
   exists. Offline lifecycle tests do not satisfy these release requirements.

Windows runtime comparison, vehicle communication, electrical testing and full
J2534 conformance remain outstanding. Firmware updating, Wine and SocketCAN are out of scope.
