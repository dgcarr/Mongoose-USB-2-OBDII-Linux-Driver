# Changelog

The format follows Keep a Changelog. There have been no releases; everything is under Unreleased.

## Unreleased (0.1.0)

### Added
- `mongoose-socketcan`, a bridge that makes the adapter a SocketCAN interface (normally a `vcan`), so can-utils,
  Wireshark, python-can, udsoncan and the kernel's ISO-TP sockets work with it. Listen-only unless `--transmit` is
  given; remote and error frames are refused. Tested end to end over `vcan` against a simulated ECU (including a
  multi-frame UDS read through a kernel ISO-TP socket) and on the adapter with no vehicle; not yet run on a car.
- `docs/VOLVO.md`: from clone to reading live data, trouble codes and the VIN, with notes for Volvo owners. A test
  runs its Python examples verbatim through the bridge.
- Licensed under LGPL-2.1-or-later (`LICENSE`); the Arch, DEB and RPM metadata name the licence and maintainer.
- A J2534-1 04.04 library for the MongoosePro JLR adapter over the kernel's cdc_acm serial port: raw CAN
  (11-bit and 29-bit) and 11-bit ISO15765, pass, block and flow-control filters, timed and queued writes with
  delivery confirmation, multi-frame receive reassembly (one conversation per source ID) and multi-frame transmit.
- Periodic messages (CAN), `LOOPBACK`, the buffer and filter-table IOCTLs, and `GET_CONFIG`/`SET_CONFIG` for the CAN
  and ISO15765 parameters, with the vendor's ranges (recovered from the vendor DLL with Ghidra; see `PROTOCOL.md`
  section 7e).
- `mongoose-client --script` and `--script-check`, running the same step files as the Windows reference harness,
  and the `--vehicle-*` validation modes; `mongoose-diag` for adapter-level bring-up.
- Packaging: install rules, a pkg-config file, a CMake package (`mongoose::j2534`), man pages, a udev rule
  (`uaccess` plus ModemManager ignore), an Arch `PKGBUILD` and CPack DEB, and a GitHub Actions workflow.
- Hardening by default: stack protector, `_FORTIFY_SOURCE=3`, stack-clash protection, CET, RELRO with BIND_NOW,
  PIE and a non-executable stack. `docs/USING.md` describes the API for callers.

### Changed
- ISO15765 transmit is no longer limited to a single frame: an ID plus 1..4095 bytes goes out in one command and
  the adapter segments it.
- Transmit confirmations are paired with their request by sequence number, as the vendor does, instead of by
  arrival order, and only a confirmation that pairs counts toward a timed write.
- An unknown or inapplicable IOCTL ID returns `ERR_INVALID_IOCTL_ID`, as the vendor does.

### Fixed
- A `PassThruWriteMsgs` of more than one message timed out on the adapter and left the device needing a reopen. The
  data commands counted down the messages still to send (3, 2, 1), as the vendor does, but the vendor sends a
  whole call before waiting for the firmware's single answer, and the driver waited after each command; a count
  above 1 is never answered alone. Each message is now a one-message transaction. Found by the SocketCAN bridge.
- Connect and `DATA_RATE` refuse a bit rate outside the vendor's list of 18 CAN rates, with `ERR_INVALID_BAUDRATE` / `ERR_INVALID_IOCTL_VALUE`, instead of passing any nonzero rate to the adapter.
- Script runner: a line longer than its buffer is refused instead of being read as several, so the tail of a long
  comment can no longer run as a command; a line with an embedded NUL is refused too. `--gap` rejects a value that is
  not a whole number or overflows, instead of becoming zero.
- `--vehicle-ignition` starts out treating the bus as quiet and sends nothing until it has seen a frame.
- The raw-CAN lifecycle check bounds its whole reply search by one deadline; the ISO soak stops when the adapter is gone.
- TTY writes stop at the caller's deadline between short writes, and exclusive mode is cleared on close. The libusb
  backend reserves its bookkeeping before detaching a kernel driver. The transmit probe counts its own confirmation.
- Writes validate the entire batch before sending, count confirmations per call on every exit, and use one uncapped deadline; outbound status `0x101` returns `ERR_BUFFER_FULL`.
- Periodic teardown clears entries left by ambiguous adds, and a failed host insertion removes the firmware entry.
- ISO15765 flow-control filters with a NULL flow-control message return `ERR_NULL_PARAMETER`.
- Multi-frame replies from two ECUs answering one functional request no longer lose one of them.
- An ISO15765 channel is closed on its own node (`0x0601`), not the CAN node.
- The bench transmit probes sent an un-framed request that no ECU could answer.

### Validation
- Live on one 2017 Volvo XC60: an hour of receive and requests on raw CAN and on ISO15765, 100 lifecycle cycles
  on each, configuration and ISO-TP timing, loopback, periodic messages, multi-frame transmit, and an
  ignition-off, bus-sleep and wake cycle. See `docs/VALIDATION.md`.
- 17 tests, run under ASan/UBSan and ThreadSanitizer, with warnings as errors on GCC and Clang.

### Not done
- K-line, J1850, the pin-switched `*_PS` protocols, 29-bit ISO15765, programming-voltage output: no hardware.
- Use at your own risk; see the README.
