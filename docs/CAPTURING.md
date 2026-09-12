# Reference captures

## Windows laptop

Install the vendor MongoosePro JLR package and Wireshark with USBPcap. The supplied
DLL is **32-bit**, even though the installer is labelled x64. Use a Win32 harness.

Build from an x86 Visual Studio developer shell:

```bat
cl /W4 /Iinclude tools\windows_reference.c /Fe:mongoose-reference.exe
```

Or cross-compile on Linux (the executable needs only Windows system DLLs):

```sh
sudo apt-get install gcc-mingw-w64-i686-posix
i686-w64-mingw32-gcc -std=c11 -Wall -Wextra -Werror -Iinclude tools/windows_reference.c -o mongoose-reference.exe
```

An already cross-compiled copy from this session is in `build/windows/` (ignored
build output, so regenerate after cloning). The capture kit contains no vendor binaries.
Run `tools/windows_metadata.ps1` and save its output. In Wireshark, start a USBPcap
capture on the adapter's root hub before running the harness. Use an absolute DLL path:

```bat
mongoose-reference.exe C:\path\to\monpj432.dll --cycles 3 > windows-open.log
```

First capture USB-only open/version/close. Repeat with the adapter on the vehicle,
recording power/ignition state, model/year, firmware version and driver hash. Keep the
pcapng, API log, and metadata together. Windows has not been tested in this session.

For subsequent CAN research, `--can --baud N --flags N` adds Connect, a wildcard
pass filter, a one-second ReadMsgs, then cleanup. Record the verified baud/flags for
the intended bus; do not assume values from another car. Without `--tx`, the harness
never calls WriteMsgs, although connecting a CAN controller may acknowledge bus traffic.
`--tx HEX` explicitly sends one raw CAN message (four-byte big-endian ID then up to
eight data bytes). Do not supply it until the intended diagnostic request is established.

Change one parameter per capture. Preserve API call timestamps, requested counts,
returned counts, statuses, IDs and complete frames. Do not treat the baseline harness
as coverage for ISO15765, K-line, periodic messages or every IOCTL.

## Linux

```sh
build/mongoose-diag --list
build/mongoose-diag --discover --serial SERIAL --trace discovery.trace
build/mongoose-diag --open-close --serial SERIAL --trace open.trace
build/mongoose-diag --inspect --serial SERIAL --trace values.trace
```

`--inspect` reads firmware/bootloader and voltage values after opening. It never sets
voltage, opens a bus channel, or writes firmware. All modes require explicit selection.
Traces append to existing files and include control-transfer results, IN/OUT bytes,
and interrupt bytes. Use unique file names per experiment.

For an independent USB capture, use Wireshark/dumpcap on the matching `usbmonN`
interface. Membership in `wireshark` must be active in the capturing process (a new
login may be needed). This session's capture access remained unavailable, so saved
hardware traces are from the libusb layer. A cdc_acm trace records only `OUT` and
`IN`: there is no vendor control request, and the CDC notification endpoint is
consumed by the kernel, so `CONTROL`, `CONTROL_RESULT_n` and `INTERRUPT` never
appear. Capture success is not implied by a trace.

Replay format is `OUT hex` followed by one or more `IN hex` chunks. Comments start
with `#`. Preserve chunk boundaries. Only feed request/response traffic appropriate
to the chosen diagnostic mode; replay is never a fallback for PassThruOpen.
