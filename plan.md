# Remaining work

Written 2026-09-19. Status of record is `docs/VALIDATION.md`; this file only orders what is
left. Tick items off here and move the evidence into `VALIDATION.md` as each one lands.

## Goal

A fully developed Linux J2534 driver for the MongoosePro adapter (goal set 2026-09-19). That
makes everything below in scope in principle, including the protocols and hardware listed under
"Needed to reach the goal". Firmware updating, Wine and SocketCAN were out of scope before the goal
was set and stay so unless you say otherwise.

## Where we are

The wire protocol is solved and the library works on a live 2017 Volvo XC60: receive at
~2450 frames/s for an hour, timed writes confirmed, ISO15765 single-frame requests with
multi-frame VIN reassembly, 100 open/close cycles, one-hour soak. Implemented: raw CAN and
11-bit ISO15765 channels, PASS and FLOW_CONTROL filters, ReadMsgs/WriteMsgs, ReadVersion,
READ_VBATT/READ_PROG_VOLTAGE.

Not implemented (each returns `ERR_NOT_SUPPORTED` today): BLOCK filters, periodic messages,
`GET_CONFIG`/`SET_CONFIG` and every other IOCTL, 29-bit ISO15765, segmented ISO15765
transmit, `PASS_FILTER` on an ISO15765 channel, programming voltage, K-line, J1850.

Scope decision (2026-09-19): this Volvo is the only vehicle. There will be no second car, so
"other vehicles" is not a work item; anything that needs a different bus or ECU family goes in the
"Needed to reach the goal" table and is judged on whether it can be settled without one.

## Ground rules with a car attached

- Read-only traffic only. On the vehicle use `mongoose-client serial:... --vehicle-*`
  modes or new modes built the same way. 500 kbit, one channel.
- Never send OBD mode 04 (clears DTCs), UDS services other than read-only ones
  (`0x22`, `0x19`), session control, security access, routine control or any write service.
  Never send `cReflashBoard`, `cWriteSerialNumber`, `cUnprotectBootloader`,
  `cJumpToFirmware` (except the existing open path) or `cUpdateBTModule`.
- Do not connect at a wrong baud on the live bus; Windows already captured 250 k.
- One command per experiment change, evidence saved as `analysis/captures/linux-*.txt`,
  VINs redacted with `analysis/redact_vin.py`.

## Phase 0 -- housekeeping (done 2026-09-19, except item 4)

1. [x] **Sync stale docs.** `VALIDATION.md`, `NOTES.md`, `README.md` and `PROTOCOL.md` no longer
   claim the bench state is current: dated bench sections are labelled as such, the receive and
   filter caveat and the "still unproven" transmit line point at the live-vehicle evidence, and the
   open-work lists now match this file. Test count is 11 (verified: normal, ASan/UBSan and
   no-libusb builds all 11/11).
2. [x] **Fix the bench transmit tools.** `--transmit-probe` and `--can-transmit-check` now send the
   framed `02 01 00` request. Found while doing it: the probe's comment said mode 01 PID 00 but the
   bytes were an un-framed mode 09 PID 02; corrected in `VALIDATION.md`. Verified live: `0x100`
   then one `iMsgTxDone`.
3. [x] **Script runner on Linux.** `mongoose-client [serial:S] --script FILE [--gap MS]` runs the
   `tools/scripts/*.txt` files with the Windows harness's verbs and log format, and
   `--script-check` (the `script_syntax` CTest) parses all 32 with no adapter. The Windows harness
   itself was left untouched: it produced the reference captures and cannot be compile-checked
   here (no mingw). Verified live on `b2-version` and `e1-obd-mode01-pid00`.
   **First result:** e1 matches Windows step for step except that Windows delivers a TX-done
   message (`RxStatus 9`, ID only) ahead of the reply and we did not. Fixed in Phase 1 step 0.
4. [ ] **Windows availability.** Needs an answer from you: will the Windows machine and this car
   be available together? That decides how much of Phase 2 runs before Phase 1's guesses.

## Phase 1 -- the car is connected now (Linux, read-only)

Ordered cheapest and safest first. Each has a pass criterion so it can be closed. Runnable
steps can now be written as `tools/scripts` files and run with `mongoose-client --script`.

0. [x] **TX-done indication messages (ISO15765).** Done 2026-09-19. Each request is recorded
   before it is sent and the adapter's `iMsgTxDone` is paired with it, queuing the `RxStatus 9`
   message Windows delivers. Pinned in `isotp_tests`; `e1` and `e2` on the car now match the
   Windows logs line for line (timestamps, error text and VIN bytes aside). Still open: whether a
   raw CAN write should deliver one too. That needs the Windows `e3` capture (Phase 2).

1. [x] **Error and timeout semantics (e4, h2).** Done 2026-09-19. Unsupported PID: silence, read
   returns the TX-done message with result 9. Unserviceable request: `7F 09 12` and `7F 22 31`
   arrive as ordinary messages. Pinned in `isotp_tests`, not `channel_tests` (there is no ISO15765
   scenario there; adding one is a Phase 3 test item).
2. [x] **Physical addressing and other ECUs (h1).** Done: `0x7E0`->`0x7E8` and `0x7E1`->`0x7E9`,
   two flow-control filters on one channel, physical VIN reassembled. Only those two ECUs answer
   on the 0x7E0 range; other ECUs (UDS-only modules on the Volvo's other buses) were not tried.
3. [x] **Multi-frame receive beyond the VIN (h3).** Done 2026-09-19, and it found a bug: two ECUs'
   interleaved multi-frame replies lost one message because the reassembler had a single shared
   assembly. Now one conversation per source ID (see `VALIDATION.md`). Left: mode 02 freeze frame,
   and a multi-frame DTC list, which needs a car that has stored codes (this one has none).
4. [x] **Padding flag (h4, h5).** Done. Padding off: the adapter transmits and confirms, and this
   car's ECUs ignore the short frame (proved on raw CAN with a 3-byte versus 8-byte request). The
   library needs no change; requests on this car need `ISO15765_FRAME_PAD`. What byte the adapter pads
   with is unobserved (no second node to watch the bus).
5. [x] **BLOCK filters (table selector 1, type 2).** Done and confirmed on the car (h6): `0x7E8`
   disappears, `0x7E9` and 70 other IDs stay, removal from table 1 works. `channel_tests` pins the
   wire bytes. `ERR_NOT_SUPPORTED` remains for BLOCK on an ISO15765 channel by design.
6. **`PASS_FILTER` on an ISO15765 channel.** Try it on the wire; record whether the
   firmware accepts it and what it changes. Implement or keep refusing, by result.
7. [x] **Clear-buffer IOCTLs (h7).** Done: RX, TX, MSG_FILTERS and PERIODIC_MSGS. On the car
   `CLEAR_RX_BUFFER` discarded a second of queued frames (next frame 2.0 s newer) and
   `CLEAR_MSG_FILTERS` stopped the flow. `CLEAR_PERIODIC_MSGS` is a no-op until step 11.
8. [x] **`GET_CONFIG`.** The firmware sweep (`mongoose-diag --value-sweep`, `PROTOCOL.md` 7d) found the
   selectors; the vendor's ID-to-selector map then came from decompiling the DLL with Ghidra (new
   `analysis/ExtractCallers.java`, `analysis/decompiled/config/`), not from a Windows capture, and is in
   `PROTOCOL.md` 7e. Implemented for CAN and ISO15765 (`src/config.cpp`), pinned by `config_tests`
   and wire scenarios in `channel_tests`. Live check on the car: see `VALIDATION.md`.
9. [x] **`SET_CONFIG` and ISO-TP timing.** Implemented with the vendor's ranges. Timing is observed on
   the car through message timestamps (the start-of-message and the completed message bracket the
   consecutive frames, so STMIN shows as elapsed time); see `VALIDATION.md`. Original description: The `0x010e` indication reports the adapter's own
   flow-control frame with block size, STmin and a device timestamp, so timing can be
   observed on one channel with no second tap. Set `ISO15765_BS`/`ISO15765_STMIN`, repeat a
   multi-frame request (mode 09 PID 02 or 04), read the `0x010e` body and the
   consecutive-frame timestamps. Pass: BS/STMIN changes visible on the wire, values within
   J2534 ranges, N_Bs behaviour when a request's flow-control filter is missing recorded.
   Gate: do step 8 first, and reset any changed value before closing the channel.
   `LOOPBACK` is a separate item (see step 10).
10. [x] **`LOOPBACK`.** Host-side flag (the vendor never sends it to the firmware) that queues the
    frame back as a received message when a transmit is confirmed; implemented for CAN and ISO15765.
    `DATA_RATE` can be set (1..1 Mbit) but is **not** exercised on the car. Original description: `LOOPBACK` = 1 makes our own transmits
    appear in the receive queue; confirm with a request to `0x7DF`. `DATA_RATE` change
    after connect is *not* attempted on the car.
11. [x] **Periodic messages (table selector 4).** The stop and clear layouts were the missing piece and
    came from the decompile (senders `1000d3c0`, `1000d4d0`); implemented for CAN, ten per channel,
    cleared before the channel closes. Live check: see `VALIDATION.md`. Original description: Send-side layout is in builder `1000d220`;
    the stop layout is not recorded. Read `senders/` for the stop path (static, no car)
    before touching the wire. Then run a periodic mode 01 PID 00 request to `0x7DF` at
    1 s and stop it. Pass: replies arrive at the period, stop ends them, the ID is freed,
    and teardown with a live periodic stops it too.
12. [x] **Segmented ISO15765 transmit.** The vendor does not segment: its frame builder `1006b090`
    copies the message as given, so the adapter does. The single-frame limit is lifted (ID plus 1..4095
    bytes in one command). Live check: see `VALIDATION.md`. Original description: Get the truth from a Windows capture first (Phase 2,
    step 2). If Windows is unavailable, try one read-only UDS `0x22` request with four
    DIDs (`22 F190 F187 F18C F194`, nine bytes) to `0x7E0` and see whether the firmware
    segments it. Abort on any unexpected reply.
13. [x] **Bus silence and wake (ignition off, locked, on again).** Done 2026-09-19 with
    `--vehicle-ignition`: the bus stepped down, went quiet, woke on the same channel, no driver errors. Still
    open: a **USB unplug with traffic running** (you pull the cable) and vehicle power lost while USB stays
    connected (`eVbattLoss`). Original description:
14. [x] **Raw CAN path cycled, ISO15765 path soaked.** Done 2026-09-19: 100 raw CAN cycles, the ISO15765
    100 cycles re-run after the close-node fix, and a one-hour ISO15765 soak (3600/3600 requests answered,
    360/360 two-ECU multi-frame replies complete, memory flat).
15. [x] **Config, loopback, periodic, segmented transmit, on the car.** All confirmed; see `VALIDATION.md`.

## Phase 2 -- needs the Windows harness on the car

**Ready to run; see `docs/PHASE2-WINDOWS.md`** for the ordered batch, what to look for in each capture,
and the safeguards. The scripts (`f2b`, `g1`-`g4`, `g6`, `e5` are new; the rest already existed) parse
in the Linux runner, and `windows_reference.c`, which gained `periodic`/`stopperiodic`, was
syntax-checked against a stub `windows.h` but not compiled on Windows.

New wire evidence the Linux side cannot safely guess. Scripts in `tools/scripts/`; run with
`mongoose-reference.exe <dll> --script FILE --gap 2000`.

1. Unrun scripts: `c2-baud-125000`, `c2-proto-CAN_PS`, `c2-proto-ISO15765_PS`,
   `d3-block-filter`, `e3-raw-can-isotp-host-side`, `e4-unsupported-pid`,
   `f2-getconfig-sweep-low`, `f3-clear-buffers`. (Skip the ones Phase 1 already settles
   if Windows is not to hand; they exist to be diffed against Linux.)
2. New scripts to write: `e5-segmented-tx` (the read-only `0x22` request above),
   `g1-periodic-start-stop`, `g2-setconfig-stmin-bs` (mirror of Phase 1 step 9),
   `g3-pass-filter-iso15765`, `g4-loopback`.
3. Diff each against the Linux run of the same script (Phase 0 step 3) and fold the
   differences into `PROTOCOL.md` section 7b/7c.

## Phase 3 -- bench, no car

- [x] Fuzz the new parsers as they land (BLOCK filter builder, periodic encoder, config table,
  sequence-paired receiver) in `tests/fuzz_codec.cpp`. Smoke-tested only; give it a long run.
- [x] Unit and wire tests for every new form (`channel_tests`, `config_tests`, `isotp_tests`). Note the
  wire forms come from the vendor decompile and the car, not from captured Windows frames.
- J2534 04.04 conformance pass over the 14 exports: error codes, `GetLastError` strings,
  `PASSTHRU_MSG` layout and `Timestamp`/`ExtraDataIndex`, ReadMsgs/WriteMsgs count and
  timeout semantics, `Ioctl` argument structs. Write it as a CTest entry.
- Filter-table maximum (stopping short of allocator exhaustion is still the rule) and
  extended-address ISO15765 reassembly only if a capture ever exercises it.
- Packaging: check `packaging/60-mongoose-j2534.rules`, install target, a `pkg-config`
  file and the J2534 registration story for Linux clients.
- Decide the fate of the libusb backend now that cdc_acm is proven under load.

## Phase 4 -- Linux standards: packaging and hardening (chosen 2026-09-19)

You asked that the driver meet Linux driver standards and to consider Rust. The options were a SocketCAN
bridge, an in-kernel Rust driver, a Rust rewrite of the core, or packaging and hardening only; **you chose
packaging and hardening only.** So this stays a userspace C++ library, and none of the kernel or Rust work is
planned. (For the record: this kernel, 7.2.3, has `CONFIG_RUST=y` and `rustc` is installed, but there are no
kernel headers here, and a kernel module could crash the machine while it is on the car.)

- [x] Risk notice ("use at your own risk", no warranty, not affiliated, not J2534-certified) in `README.md`
  and `docs/USING.md`.
- [x] Consumer surface: `docs/USING.md`, a compiled and car-tested example (`examples/obd_request.c`), a
  pkg-config file, install of docs and header.
- [x] Hardening flags (stack protector, FORTIFY_SOURCE=3, stack-clash, CET, RELRO/BIND_NOW, PIE, noexecstack),
  on by default (`MONGOOSE_HARDENING`, off under the sanitizers), verified with `readelf`/`nm` on the library
  and a tool. The default build type is now RelWithDebInfo, since FORTIFY needs optimisation. The hardened
  build was smoke-tested on the car: 100/100 cycles on each path, the example, and 21 config reads.
- [x] A warning-free optimised build. The optimised build found six warnings, now fixed: dead code, three
  `client.c` conversions and a shadow, and a GCC false positive in a test.
- [x] udev rule: `uaccess` for the active seat user plus the ModemManager ignore, passes `udevadm verify`;
  installs to `lib/udev/rules.d` under the prefix (relative, so `--prefix` and `DESTDIR` work; a packager can
  override `MONGOOSE_UDEV_RULES_DIR`). Found on the way: the first version baked an absolute path.
- [x] Man pages: `mongoose-client(1)`, `mongoose-diag(1)`, `libmongoose_j2534(7)`, lint-clean under `groff -ww`,
  installed to `man1`/`man7`. The install tree was checked for prefix and DESTDIR use and has no build-dir RUNPATH.
- [x] CMake package config: `find_package(mongoose-j2534 0.1)` gives `mongoose::j2534`, tested with a real
  consumer project, and a version request for 0.2 is refused. Found on the way: the exported name was wrong
  (`mongoose::mongoose_j2534`), and the include path was listed twice; both fixed.
- [x] Distro packages: the Arch `PKGBUILD` (`packaging/arch/`) builds with `makepkg` including its `check()`
  step (12/12), is stripped, has no RPATH and keeps RELRO/BIND_NOW/PIE/canary; a `.deb` is generated by CPack
  with the right architecture and a libusb dependency only when built with libusb. Not tested: RPM (no
  `rpmbuild` here) and installing either package. Placeholders: maintainer and licence.
- [x] CI workflow (`.github/workflows/ci.yml`): gcc and clang with warnings as errors, ASan/UBSan, TSan, a tty-only
  build that must not link libusb, packaging/install/consumer/man/udev/hardening checks, a DEB, and a fuzz
  smoke test. The YAML parses and the steps were run locally; the workflow itself has not run on GitHub. Clang
  found one more warning (an always-false `CHECK(!"...")` idiom), fixed.
- [x] ABI and versioning policy, thread-safety statement (the error text is per thread) in `docs/USING.md` and
  the man page, and a `CHANGELOG.md`.
- [ ] **CI is red on one job (open, 2026-09-19).** First run (commit `f53d74b`): 6 of 7 jobs passed; the
  **ThreadSanitizer** job failed 6 of 12 tests on GitHub's Ubuntu 24.04 / GCC 13 with "unlock of an unlocked mutex"
  in `Session::command` (`std::timed_mutex` unlock; `try_lock_until` on a steady clock at `src/session.cpp:45`).
  Diagnosis: GCC 13's `libtsan.so.2` has no interceptor for `pthread_mutex_clocklock`, which libstdc++ uses there, so
  TSan never sees the lock. Local GCC 16 does not show it, and the tests are clean locally. **The first fix, `92d7af4`
  (`-D_GLIBCXX_USE_PTHREAD_MUTEX_CLOCKLOCK=0`), does not work**: libstdc++'s own `c++config.h` unconditionally
  `#define`s that macro to 1, which overrides the command-line flag. It was pushed and the run
  (`35441703039`) failed identically; I only proved it did not break the build, not that it fixed anything.
  Options, none tried yet: (a) a TSan suppression file (`mutex:std::timed_mutex::unlock` via
  `TSAN_OPTIONS=suppressions=`), the least invasive; (b) build the TSan job with clang; (c) a newer GCC (`g++-14`)
  in the job; (d) change `Session` to avoid `try_lock_until` on a steady clock (not recommended: wall-clock jumps
  would then affect command waits). I wanted to reproduce it first with `pkexec docker run ubuntu:24.04` (Docker
  needs root here) and you declined that prompt, so nothing is reproduced. Check with `gh run list` and
  `gh run view <id> --log-failed`. Also cosmetic: `actions/checkout@v4` warns about Node 20 deprecation.
- [ ] **Needs you:** no `LICENSE` file exists. Choosing a licence is yours to decide; packages need one.

## Decision on the remaining protocols (2026-09-19)

Asked whether to implement 29-bit ISO15765, K-line and J1850 from the vendor decompile without any way to test
them, you chose to **leave them unsupported**. They stay `ERR_NOT_SUPPORTED`, and `docs/USING.md` lists what
would unblock each. The goal "fully developed for all protocols" therefore cannot be met by this project as it
stands; what is complete is CAN and 11-bit ISO15765, validated on the car.

## Needed to reach the goal, and not available yet

Each needs something beyond this Volvo. Now in scope because of the goal, but they cannot start
until the hardware or captures exist; recorded so nobody assumes they are quietly covered.
Say which you can supply and they move into a phase.

| Item | Needs |
|---|---|
| 29-bit ISO15765 and J1939 | A 29-bit ECU or a second CAN node/simulator; no captured filter layout |
| K-line (ISO9141/14230), J1850 | A vehicle or bench ECU with those buses, and Windows captures first |
| Programming voltage output | Scope or meter on the connector; explicit electrical sign-off |
| Independent frame count | A second CAN logger on the same bus to prove zero loss |

## Suggested order

Phases 0 and 1 are done except step 13 (needs you: switch the ignition off and on, and pull the USB cable,
while a channel is open and streaming). Phase 2 (Windows) is now an optional cross-check, not a blocker.
What is left that needs no new hardware: the J2534 04.04 conformance pass, packaging, a long fuzz run,
and a decision on the libusb backend. What needs hardware this project does not have is the "Needed to
reach the goal" table below.
