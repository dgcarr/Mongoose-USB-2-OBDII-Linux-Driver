# MongoosePro JLR Linux driver

Experimental native C++20 J2534 library for USB `18e1:0104`, talking to the adapter as
an ordinary `cdc_acm` serial device.
**Linux discovery, device open/close and CAN channel setup now work on the adapter. Vehicle diagnostics
are not implemented yet.** See [validation and remaining work](docs/VALIDATION.md).

Implemented: two transports (cdc_acm by default, libusb optional), scoped device
ownership, asynchronous reception, validated length/XOR
framing, serialized commands, startup/cleanup, native Open/Close/ReadVersion,
voltage-reading IOCTLs, CAN Connect/Disconnect, replay tests and all 14 core ABI exports.
CAN supports flags 0 or CAN_29BIT_ID, with one channel per adapter. Message I/O,
filters, periodic messages and ISO15765 remain unsupported; channel setup alone
does not mean vehicle diagnostics work.

## Build and test

```sh
sudo apt-get install cmake ninja-build g++
sudo apt-get install libusb-1.0-0-dev   # optional: only for the libusb backend
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
build/mongoose-diag --list
build/mongoose-diag --discover --trace discovery.trace
build/mongoose-client
# Explicit CAN setup/reconnect check; never calls WriteMsgs:
build/mongoose-client serial:SERIAL --can-lifecycle
```

`PassThruOpen(NULL, ...)` requires exactly one matching adapter. Use `serial:SERIAL`
for explicit selection in the native client/API, or `--serial SERIAL` in the diagnostic
executable. All scalar ABI fields are 32-bit; pointer-containing lists use native
pointer alignment. GetLastError is thread-local and retains the last failure.

For sanitizer checks:

```sh
cmake -S . -B build-asan -G Ninja -DMONGOOSE_SANITIZERS=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

A Clang/libFuzzer target is available with `-DMONGOOSE_FUZZ=ON`.

## Install

```sh
sudo cmake --install build
sudo install -m 0644 packaging/60-mongoose-j2534.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo ldconfig
```

`-DMONGOOSE_LIBUSB=OFF` builds a cdc_acm-only driver that configures and tests with no
libusb installed at all.

## Selecting a device

`PassThruOpen`'s name, and `mongoose-diag --device`, take:

| Selector | Meaning |
|---|---|
| `NULL` / omitted | cdc_acm, the single connected adapter |
| `serial:S` | cdc_acm, matched by USB serial |
| `tty:/dev/ttyACM0` | cdc_acm, explicit node |
| `usb:` or `usb:serial:S` | libusb backend |

There is no automatic fallback between backends: falling back would silently detach
`cdc_acm` and make failures irreproducible.

Reconnect the adapter. The default cdc_acm backend needs **no permissions setup**:
`/dev/ttyACM*` is already `root:dialout 0660` and systemd adds an ACL for the active
local user, so membership of `dialout` is only needed for headless or remote sessions.
The udev rule no longer grants access at all -- its sole job is keeping ModemManager
from probing the adapter's CDC port. Install it wherever ModemManager runs: without it
MM may send AT commands to the adapter on plug-in. The framer resyncs, so this degrades
rather than breaks, but it is worth avoiding. The libusb backend still needs raw USB
access and therefore its own rule; it is a fallback, not the normal path.

One process may own an adapter at a time: the cdc_acm backend takes both `flock` and
`TIOCEXCL`, so a second instance -- or `minicom`, or a stray probe -- fails with
`ERR_DEVICE_IN_USE`. The two backends do **not** exclude each other, because they lock
different objects. Running both against one adapter is unsupported: libusb detaches
`cdc_acm`, so the libusb side wins and the tty session fails with a disconnect.

A response timeout invalidates the session; close and reopen before
retrying. A command that expires before its write reaches the wire returns `ERR_TIMEOUT`
without invalidating the session. Every accepted timeout, down to the 1 ms minimum,
reaches the adapter: the write budget rounds up to whole milliseconds, because zero is
out of contract for every transport -- libusb reads it as "no timeout" and `poll(2)` as
"expire immediately". A write may therefore overrun the deadline by under a millisecond;
the wait for the response honours it exactly.

`cpack --config build/CPackConfig.cmake` packages only installed library, headers,
tools and documentation; vendor files and research binaries are excluded.

## Research and Windows comparison

- [Windows capture instructions](docs/CAPTURING.md) and `tools/windows_reference.c`
- [Protocol reference](PROTOCOL.md) and [research status](NOTES.md)
- [Ghidra reproduction](analysis/REPRODUCE.md) and [sender index](analysis/SENDERS.md)

The old probe script is a historical speculative experiment, not the driver; it is
kept at `analysis/history/probe.py`.
Windows reference captures now cover the 2017 Volvo XC60 D5 AWD. Linux CAN channel
setup has been exercised on USB power only, without a vehicle or external 12 V.
Message I/O and Linux vehicle validation remain next work.

CAN lifecycle calls are serialized per device. Handles increase without reuse and
are invalidated on disconnect or device close. A rejected pin setup is rolled back
with CloseChannel; failed rollback or ambiguous transport failure requires device
close/reopen. Unsupported channel APIs return ERR_NOT_SUPPORTED for a live channel
and ERR_INVALID_CHANNEL_ID for an invalid one, with output counts/IDs cleared.
