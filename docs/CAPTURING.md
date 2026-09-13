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
pcapng, API log, and metadata together. Completed Windows runs and their limits are recorded in WINDOWS-FINDINGS.md.

For subsequent CAN research, `--can --baud N --flags N` adds Connect, a wildcard
pass filter, a one-second ReadMsgs, then cleanup. Record the verified baud/flags for
the intended bus; do not assume values from another car. Without `--tx`, the harness
never calls WriteMsgs, although connecting a CAN controller may acknowledge bus traffic.
`--tx HEX` explicitly sends one raw CAN message (four-byte big-endian ID then up to
eight data bytes). Do not supply it until the intended diagnostic request is established.

Change one parameter per capture. Preserve API call timestamps, requested counts,
returned counts, statuses, IDs and complete frames. Do not treat the baseline harness
as coverage for ISO15765, K-line, periodic messages or every IOCTL.

### Step scripts

Beyond those flags the harness takes `--script FILE`, one J2534 call per line, so an
experiment is a data file rather than another flag. `tools/scripts/` holds one file per
test with `tools/scripts/README.md` describing the grammar. `--gap MS` sleeps between
steps; two seconds makes wire frames group visibly by originating call, which is what
lets a capture be read without clock-sync machinery.

```bat
mongoose-reference.exe C:\path\to\monpj432.dll --script tools\scripts\b1-open-close.txt --gap 2000
```

Handles are names assigned by `open` and `connect`; a bare number is passed through
verbatim so error-path scripts can hand the DLL a deliberately invalid ID. Scripts named
`-expectfail` are expected to exit 1 — the returned error code is the result being
recorded. `PassThruSetProgrammingVoltage` is deliberately not implemented: driving
voltage onto an OBD pin of a live vehicle is not a read-only act.

`tools\capture.ps1 -Script FILE` runs one script between USBPcap start and stop, writing
`wire.pcap`, `api.log` and a `metadata.txt` carrying the DLL and driver hashes into
`analysis/captures/windows/<timestamp>-<name>/`. Capture worked without elevation in the recorded setup; the
ignition state and vehicle fields in the metadata are filled in by hand.

USBPcap registers itself as an `UpperFilters` entry on the USB device class, so the
filter only attaches to root hubs after a **reboot**. Before that reboot
`USBPcapCMD --extcap-interfaces` lists nothing and every capture file is empty.

`tools\vendor_debug_log.ps1` toggles `DebugEnable` on the vendor's PassThru registry key.
`monpj432.dll` carries the strings `DebugEnable`, `C:\DrewTech\Logs` and a log filename
template, so the vendor may be able to write its own log alongside the capture. The captured DWORD/string trials produced no logs; do not repeat registry guesses
without tracing the gate (see WINDOWS-FINDINGS.md).

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

### Linux CAN setup only

`build/mongoose-client serial:SERIAL --can-lifecycle` uses the production J2534
library to open, read version, connect/disconnect at 500k and 250k flags zero,
then 500k with CAN_29BIT_ID, and close. It does not call WriteMsgs. This has
succeeded with USB power alone, but connecting on a live bus may acknowledge
traffic. Select the intended adapter explicitly and record power/vehicle state.
For API-level wire evidence without a USB capture, use `strace -f -ttt -xx -s 8192
-e trace=read,write,openat,close -o lifecycle.strace` before the client command.
Keep only the selected tty's syscall buffers when deriving OUT/IN trace records;
label these as syscall traces, never USB captures.
