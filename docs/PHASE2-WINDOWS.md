# Phase 2: the Windows capture session

> **Status update, 2026-09-19 (later).** This sheet was written when the Windows captures looked
> necessary for `GET_CONFIG`/`SET_CONFIG`, periodic messages, `LOOPBACK` and segmented transmit. Static
> analysis of the vendor DLL (Ghidra, `analysis/ExtractCallers.java` and `ExtractByName.java`) has since
> answered those, and the driver implements them from the decompile (`PROTOCOL.md` section 7e). What the
> Windows session is still good for is **verification**: running the same scripts through the vendor DLL
> and diffing the wire against ours. Nothing on the driver's critical path waits for it now. Items 1-3
> (the config sweeps), 5 (`g4`), 7 (`g1`) and 11 (`e3`) are answered statically; 4 (`g2`) and 6 (`e5`)
> are being checked directly on Linux; 8-10 remain only as a comparison. Treat the rest of this
> document as an optional cross-check, not a blocker.

One trip to the Windows machine, with the adapter on the same Volvo and the ignition on (engine
state as you like; note it). Everything here is **read-only OBD-II on the bus** or a **volatile
channel setting** that the script restores; none of it flashes, changes a session or clears codes.
The Linux driver is at the point where it needs the vendor DLL's own frames to go further, so each
item below says what it unblocks.

## Before you go

1. **Rebuild the harness.** `tools/windows_reference.c` gained two steps, `periodic` and
   `stopperiodic`, since the captures in `analysis/captures/windows/` were taken. The change was
   only syntax-checked against a stub `windows.h` on Linux; it has not been through a Windows
   compiler. Build as in `docs/CAPTURING.md` (`cl /W4 /Iinclude tools\windows_reference.c
   /Fe:mongoose-reference.exe`). If it does not compile, the error text is all that is needed to fix it.
2. Copy the repository's `tools\scripts\` and `tools\capture.ps1`. Make sure USBPcap is attached
   (it needs the reboot noted in `CAPTURING.md`).
3. Unplug the adapter from the Linux machine and plug it into the car and the Windows machine.

## The batch, in order

Run each with `tools\capture.ps1 -Script <file>` (which writes `wire.pcap`, `api.log` and
`metadata.txt` under `analysis\captures\windows\<timestamp>-<name>\`). Fill in the ignition state
in the metadata. Gap of 2000 ms between steps, as before.

| # | Script | What it settles | Unblocks |
|---|---|---|---|
| 1 | `f2-getconfig-sweep-low` | The firmware selector the vendor sends for each SConfig ID `0x01`-`0x20`, on a CAN channel | `GET_CONFIG` / `SET_CONFIG` |
| 2 | `f2b-getconfig-sweep-high` | The same for `0x21`-`0x30`, `0x8001`, the vendor IDs | same |
| 3 | `g6-getconfig-iso15765` | The ISO-TP parameters (BS, STMIN, BS_TX, STMIN_TX, WFT_MAX, pad value, N_* timeouts) on an ISO15765 channel | ISO15765 timing |
| 4 | `g2-setconfig-stmin-bs` | Whether setting BS=2, STMIN=20 changes the adapter's flow-control frame (`0x010e` indication shows `30 02 14`). Restores 0/0 | ISO15765 timing |
| 5 | `g4-loopback` | LOOPBACK (SConfig 3): the selector, and that our own request comes back. Restores 0 | `LOOPBACK` |
| 6 | `e5-segmented-tx` | Who segments a multi-frame **transmit**, and the wire form | segmented ISO15765 transmit |
| 7 | `g1-periodic-start-stop` | The table-4 add layout and, the part we lack entirely, the stop command | periodic messages |
| 8 | `g3-pass-filter-iso15765` | Whether the vendor accepts a PASS filter on an ISO15765 channel, and how | `PASS_FILTER` on ISO15765 |
| 9 | `f3-clear-buffers` | The vendor's wire frames for the clear IOCTLs, to compare with ours | conformance check |
| 10 | `d3-block-filter`, `e4-unsupported-pid` | Windows side of two things we have only run on Linux | conformance check |
| 11 | `e3-raw-can-isotp-host-side` | Whether a **raw CAN** write also delivers a transmit-done message | raw CAN transmit indication |

Items 1-5 are the important ones; 6 and 7 are next; 8-11 are cheap once the rig is set up.
If time is short, do 1, 3, 4, then 6 and 7.

## What to look at in each

- **1-3 (`getconfig`).** In `api.log` each parameter prints `config parameter=0x.. value=0x..`. In the
  pcap the vendor sends one `cGetValue` (opcode `0x0c`) per parameter with a four-byte selector at
  body+12. The pairing of the two, ID to selector, is the result. Compare with the firmware values in
  `PROTOCOL.md` section 7d (for example `DATA_RATE` should read 500000 and the selector should be `0x04`).
  A rejected ID prints a nonzero result and is itself a finding.
- **4 (`g2`).** In the pcap look for `cIndication` (`0x0a`) with code `0x010e`; its body carries the
  flow-control frame. Expect `30 00 00` in the first and third VIN exchanges and `30 02 14` in the
  second. Also note the timestamps of the consecutive frames in the second exchange: with STMIN 20 ms
  they should be spaced by about 20 ms. **The VIN appears in the reply**; redact with
  `analysis/redact_vin.py` before committing.
- **5 (`g4`).** With LOOPBACK 0 the read is empty. With 1, a message with our `0x7DF` frame should
  come back. Note the RxStatus bits on it (a transmit indication is normally `TX_MSG_TYPE`).
- **6 (`e5`).** In the pcap: one `cOutboundData` carrying all nine service bytes (the adapter
  segments), or several frames (the DLL segments and the ECU's flow control is handled by the host)?
  Also note whether a `0x010e` flow-control indication appears. If the ECU rejects the request the
  reply is `7F 22 xx`, which is still a valid result. The reply may contain the VIN: redact it.
- **7 (`g1`).** Read the table-add frame (selector 4) against `PROTOCOL.md` section 3 (`1000d220`:
  interval at +16, TxFlags at +20, size at +24, data at +25), and find the frame sent by
  `stopperiodic`. That frame is the missing piece. Replies should arrive about once a second while it
  runs and stop after.

## Safeguards

- **Never** add `setconfig` for `0xC002`-`0xC00A` (`NON_VOLATILE_STORE_*`): those write non-volatile
  memory. None of the scripts above touch them; do not edit one to.
- Do not run scripts other than those listed without reading them first. `PassThruSetProgrammingVoltage`
  has no step on purpose.
- The two scripts that change a setting (`g2`, `g4`) restore it before closing. Whether a channel
  setting survives closing the channel is **not established** (it is probably volatile and per channel,
  but nothing has shown it). If a run aborts between the set and the restore, do not assume it is
  cleared: open a channel again and run `getconfig` for the parameter, and `setconfig` it back to the
  default (`0x1e`=0, `0x1f`=0, `0x03`=0) by hand. Reading it back also answers the persistence question,
  so note what you find.

## When you are back

Plug the adapter back into the Linux machine, commit the new `analysis/captures/windows/*` folders
(VINs redacted), and tell me. Then I will:

1. read the wire captures and turn the findings into `PROTOCOL.md` section 7b/7e;
2. implement `GET_CONFIG` / `SET_CONFIG` with the vendor's real ID-to-selector map, then `LOOPBACK`
   and ISO15765 STMIN/BS, and check them on the car against the same scripts run through
   `mongoose-client --script`;
3. implement periodic messages and, if `e5` shows it is possible, segmented transmit.
