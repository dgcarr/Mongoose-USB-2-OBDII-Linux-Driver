# MongoosePro JLR Linux driver

Experimental native C++20/libusb J2534 library for USB `18e1:0104`.
**Linux discovery and device open/close now work on the adapter. Vehicle diagnostics
are not implemented yet.** See [validation and remaining work](docs/VALIDATION.md).

Implemented: scoped USB ownership, asynchronous reception, validated length/XOR
framing, serialized commands, startup/cleanup, native Open/Close/ReadVersion,
voltage-reading IOCTLs, replay tests and all 14 core ABI exports. Connect and other
unfinished operations return errors; exports alone do not mean protocol support.

## Build and test

```sh
sudo apt-get install cmake ninja-build g++ libusb-1.0-0-dev
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
build/mongoose-diag --list
build/mongoose-diag --discover --trace discovery.trace
build/mongoose-client
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

Reconnect the adapter. The narrow rule grants `plugdev` and active-seat access;
new group membership may require login again. No global CDC blacklist is needed.
One process may own an adapter at a time. Normal cleanup releases interfaces and
reattaches CDC. A response timeout invalidates the session; close and reopen before
retrying. A command that expires before its write reaches the wire returns `ERR_TIMEOUT`
without invalidating the session. Every accepted timeout, down to the 1 ms minimum,
reaches the adapter: the write budget rounds up to whole milliseconds so libusb is never
handed a zero, which it would read as "no timeout". A write may therefore overrun the
deadline by under a millisecond; the wait for the response honours it exactly.

`cpack --config build/CPackConfig.cmake` packages only installed library, headers,
tools and documentation; vendor files and research binaries are excluded.

## Research and Windows comparison

- [Windows capture instructions](docs/CAPTURING.md) and `tools/windows_reference.c`
- [Protocol reference](PROTOCOL.md) and [research status](NOTES.md)
- [Ghidra reproduction](analysis/REPRODUCE.md) and [sender index](analysis/SENDERS.md)

The old probe script is a historical speculative experiment, not the driver; it is
kept at `analysis/history/probe.py`.
The next vehicle target is a 2017 Volvo XC60 D5 AWD. Reference captures will be made
on a separate Windows laptop; no vehicle was connected during current testing.
