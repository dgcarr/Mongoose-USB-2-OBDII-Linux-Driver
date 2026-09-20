# Using the library

> **Use at your own risk.** This is an unofficial driver, developed independently and reverse-engineered
> from the vendor's software, and it has been tested on **one** vehicle and one adapter. It comes with no
> warranty of any kind. Sending messages to a vehicle's networks can, if done wrongly, set fault codes, disable
> or misconfigure systems, drain the battery, or damage the vehicle or the adapter. You alone are responsible
> for what you send, for having the right to send it, and for any result. The `--vehicle-*` modes of the bundled
> client and the bundled capture scripts send read-only OBD-II requests (a few change a channel setting and
> restore it), but the script runner will run any script you give it and the J2534 API does not guard against
> anything else. This project is not affiliated
> with or endorsed by Drew Technologies, Opus IVS, Jaguar Land Rover or any vehicle maker, and it is not
> J2534-certified.

`libmongoose_j2534` is a J2534-1 04.04 pass-thru library for the MongoosePro JLR adapter (USB `18e1:0104`).
It exports the 14 standard `PassThru*` functions with the C ABI in `include/mongoose/j2534.h`, and talks to
the adapter through the kernel's `cdc_acm` serial device: no vendor SDK, no libusb, no root.

**What it does today:** raw CAN (11-bit and 29-bit) and ISO15765 (11-bit), including multi-frame receive
and transmit; pass, block and flow-control filters; periodic messages (CAN); `GET_CONFIG`/`SET_CONFIG`
for the CAN and ISO15765 parameters; the buffer IOCTLs; `LOOPBACK`; battery and programming-voltage
readings. **What it does not do:** K-line (ISO 9141/14230), J1850, the pin-switched `*_PS` protocols,
29-bit ISO15765, and programming-voltage output. Those return `ERR_NOT_SUPPORTED`. See
`docs/VALIDATION.md` for what has been tested and how.

## Building against it

```sh
cmake -S . -B build && cmake --build build && sudo cmake --install build
cc app.c $(pkg-config --cflags --libs mongoose-j2534) -o app
```

or `dlopen("libmongoose_j2534.so.0", RTLD_NOW)` and resolve the `PassThru*` symbols. If you want SocketCAN rather
than J2534, run `mongoose-socketcan` instead (see `docs/VOLVO.md`): it makes the adapter a CAN interface for
can-utils, python-can and the kernel's ISO-TP sockets. The runnable example
is `examples/obd_request.c` (built as `mongoose-example-obd`). There is no registry on Linux: a J2534 client
finds the library by path.

## Opening the adapter

`PassThruOpen(name, &device)`; `name` is one of

| Name | Meaning |
|---|---|
| `NULL` | the single connected adapter |
| `serial:AOLHE0000003666A` | the adapter with that serial number |
| `tty:` / `tty:/dev/ttyACM0` / `tty:serial:S` | the `cdc_acm` backend explicitly (the default) |
| `usb:` / `usb:serial:S` | the libusb backend, if built in; needs access to the USB device node |

Anything else is `ERR_FAILED`. `mongoose-diag --list` shows what is connected.

One process at a time may have the adapter open; a second gets `ERR_DEVICE_IN_USE` (the library takes an
`flock` and `TIOCEXCL` on the tty). If ModemManager is installed, put `packaging/60-mongoose-j2534.rules` in
`/etc/udev/rules.d/` so it does not probe the adapter's serial port. Access to `/dev/ttyACM*` normally comes
from the `dialout` group or systemd's per-seat ACL; no special rule is needed for that.

## Channels

`PassThruConnect(device, protocol, flags, baud, &channel)`. Only **one** CAN-family channel can be open on the
adapter at a time (it has a single CAN controller): a second `Connect` is `ERR_CHANNEL_IN_USE`. Flags: `0`, or
`CAN_29BIT_ID` on a CAN channel. `baud` must be non-zero; the library passes it to the adapter, so use the
vehicle's real rate (500000 on the car this was developed on). Connecting at the wrong rate on a live bus is not
harmless.

### Raw CAN

A message is the four-byte big-endian CAN ID followed by up to eight data bytes: `DataSize` 4..12. There is no
ISO-TP layer, so build the PCI byte yourself: an OBD request for mode 01 PID 00 is
`00 00 07 DF 02 01 00 55 55 55 55 55`. **Pad requests to eight bytes**: the ECUs tested ignore a short frame
even though the adapter sends it.

Receive needs a filter: with none, nothing is delivered. `PASS_FILTER` with an all-zero mask and pattern passes
everything (the bus this was tested on carries about 2450 frames a second and the library keeps up);
`BLOCK_FILTER` removes what a pass filter admitted. Delivered messages carry the adapter's microsecond
timestamp. With `LOOPBACK` set (see below) each confirmed transmit also comes back as a received message with
`RxStatus` `TX_MSG_TYPE`; without it, transmits are not echoed.

### ISO15765

A message is the four-byte CAN ID followed by the service bytes, **without the ISO-TP PCI byte**: the adapter
adds it. A read-only OBD request is `00 00 07 DF 01 00` (mode 01, PID 00).

- **Pass `ISO15765_FRAME_PAD` in `TxFlags`**, on the filter messages and on every write. Without it the
  adapter sends the request unpadded and the ECUs tested do not answer.
- **Add a flow-control filter before writing** (`ERR_NO_FLOW_CONTROL` otherwise): `PassThruStartMsgFilter(
  channel, FLOW_CONTROL_FILTER, mask, pattern, flow, &id)` with the ECU's reply ID as `pattern` and the
  request ID as `flow`. The mask must cover the 11-bit ID (`0000FFFF` or `FFFFFFFF`). One filter per ECU
  that should be allowed to answer, at most 64.
- **Replies are reassembled for you.** A multi-frame reply arrives as one message (ID plus the whole payload).
  The adapter sends the flow-control frames itself.
- **Each `Read` can return three kinds of message**, told apart by `RxStatus`: `TX_MSG_TYPE|TX_DONE` (9) is
  your own request being confirmed, four ID bytes only; `START_OF_MESSAGE` (2) announces a multi-frame reply,
  four ID bytes only; `0` is a completed message.
- **Long requests work**: a message longer than one frame is sent in one call and the adapter segments it,
  honouring the ECU's flow control. The library accepts up to 4095 payload bytes, the ISO-TP maximum; the
  longest request tested on a vehicle is nine bytes (a two-frame request).
- A functional request (ID `0x7DF`) is answered by every ECU that supports it, so expect several replies; use one
  flow-control filter per ECU. No reply within your read timeout is `ERR_TIMEOUT` or `ERR_BUFFER_EMPTY`, not
  an error condition in itself: an ECU stays silent for a PID it does not support.
- `PASS_FILTER` and `BLOCK_FILTER` are refused on an ISO15765 channel (the vendor refuses them too).

## Reading and writing

`PassThruReadMsgs(channel, msgs, &count, timeout_ms)` returns what has arrived. `count` is set to the number
read. A `timeout` of 0 never waits. If fewer than requested arrive in the time, the result is `ERR_TIMEOUT` and
`count` is how many did; if none, `ERR_BUFFER_EMPTY`. `ERR_BUFFER_OVERFLOW` means messages were lost because
you were not reading fast enough.

`PassThruWriteMsgs(channel, msgs, &count, timeout_ms)`:

- `timeout` **0** queues and returns once the adapter has accepted each message; `count` is what it accepted.
- `timeout` **non-zero** blocks until the adapter reports each message was actually transmitted on the bus and
  `count` is how many it confirmed; `ERR_TIMEOUT` with a smaller `count` means it did not. This is the form to
  use: with no bus attached the adapter still accepts a frame, so only the confirmed count is proof it left.

## Periodic messages

`PassThruStartPeriodicMsg(channel, msg, &id, interval_ms)` on a CAN channel: 5..65535 ms, up to ten at a time.
`PassThruStopPeriodicMsg(channel, id)` stops one; `CLEAR_PERIODIC_MSGS` stops all. They are cleared for you when
the channel closes.

## IOCTLs

`PassThruIoctl(id, ioctl, input, output)`. Use a **device** ID for `READ_VBATT` and `READ_PROG_VOLTAGE` (output:
millivolts, rounded to 100), and a **channel** ID for the rest.

| IOCTL | Notes |
|---|---|
| `GET_CONFIG`, `SET_CONFIG` | `input` is an `SCONFIG_LIST` of 1..50 entries; `output` must be `NULL` |
| `READ_VBATT`, `READ_PROG_VOLTAGE` | device ID |
| `CLEAR_TX_BUFFER`, `CLEAR_RX_BUFFER` | the receive queue is emptied, including a half-reassembled ISO15765 reply |
| `CLEAR_PERIODIC_MSGS`, `CLEAR_MSG_FILTERS` | filters are forgotten; a channel with none sends nothing |
| anything else, including `FIVE_BAUD_INIT`, `FAST_INIT` | `ERR_INVALID_IOCTL_ID` |

Parameters for `GET_CONFIG`/`SET_CONFIG`, on both channel types unless noted (ID: range for SET):

| Parameter | ID | Range | Notes |
|---|---|---|---|
| `DATA_RATE` | 0x01 | 1..1000000 | changes the live bus timing; do not use on a vehicle |
| `LOOPBACK` | 0x03 | 0, 1 | kept in the library, not the adapter |
| `BIT_SAMPLE_POINT` | 0x17 | CAN 68..80; ISO15765 80 | |
| `SYNC_JUMP_WIDTH` | 0x18 | 0..100 | |
| `ISO15765_BS`, `ISO15765_STMIN` | 0x1E, 0x1F | 0..255 | ISO15765 only; the block size and separation time the adapter asks the ECU for |
| `BS_TX`, `STMIN_TX` | 0x22, 0x23 | 0..255 or 0xFFFF | ISO15765 only |
| `ISO15765_WFT_MAX` | 0x25 | 0..255 | ISO15765 only |
| `N_BR_MIN`, `N_CS_MIN` | 0x2A, 0x30 | 0..65535 | ISO15765 only |
| `ISO15765_PAD_VALUE` | 0x2B, also 0x10000001 | 0..255 | ISO15765 only |
| `N_AS_MAX`, `N_AR_MAX`, `N_BS_MAX`, `N_CR_MAX` | 0x2C..0x2F | 1..65535 | ISO15765 only |
| `DT_PULLUP_VALUE`, `DT_HALF_DUPLEX` | 0x10008, 0x10000007 | read only | electrical; the library will not change them |

A `SET_CONFIG` list is checked as a whole before anything is written, so a bad entry leaves the channel as it
was. An ID the channel does not have is `ERR_NOT_SUPPORTED`; a value out of range is `ERR_INVALID_IOCTL_VALUE`.
The non-volatile store IDs (`0xC002`..`0xC00A`) are refused: they write adapter memory.

## J2534 conformance notes

These deviations were reviewed and deliberately kept:

- The `CAN_ID_BOTH` (`0x800`) Connect flag is refused with `ERR_INVALID_FLAGS`; no hardware is available to validate it.
- Connect and `DATA_RATE` accept only the 18 standard CAN rates (33300 to 1000000, the vendor's list, recovered from its DLL); another rate is `ERR_INVALID_BAUDRATE`. An adapter that refuses an accepted rate is reported as `ERR_FAILED`.
- Sample-point limits (`BIT_SAMPLE_POINT` 68 to 80 on CAN, 80 only on ISO15765) are enforced although the vendor applies neither on a normal open: it is bus timing, and the driver stays on the safe side.
- Unknown or inapplicable filter types return `ERR_NOT_SUPPORTED` where the vendor returns `ERR_FAILED`.
- `ERR_NOT_UNIQUE` is never returned; duplicate flow-control filters are accepted, up to 64.
- `SET_CONFIG` validates the whole list before applying any of it, where the vendor applies entries one by one.
- The `CLEAR_*` and `READ_VBATT`/`READ_PROG_VOLTAGE` ioctls ignore stray non-NULL input or output pointers, where the vendor returns `ERR_FAILED`.
- `CLEAR_RX_BUFFER` can also discard a frame that arrived in the same USB transfer as its acknowledgement.

## Errors

`PassThruGetLastError` returns a text of at most 79 characters for the last call that failed and is cleared by
a call that succeeds. Codes follow J2534-1: `ERR_INVALID_DEVICE_ID`, `ERR_INVALID_CHANNEL_ID`,
`ERR_NULL_PARAMETER`, `ERR_INVALID_MSG`, `ERR_INVALID_FLAGS`, `ERR_NOT_SUPPORTED` for what is not implemented, and
so on. A firmware refusal comes back as `ERR_FAILED` with the adapter's own message in the text. After a
transport failure (the adapter unplugged, a response timeout) the device must be closed and reopened; calls
return `ERR_DEVICE_NOT_CONNECTED` until then.

## What is not supported, and what it would take

These return `ERR_NOT_SUPPORTED` on purpose. Code for them written from the vendor's decompiled software alone,
with nothing to test it against, would be worse than none on a vehicle bus, so they stay unsupported until
they can be checked.

| Not supported | What would unblock it |
|---|---|
| 29-bit ISO15765 and J1939 | An ECU or bench node that uses 29-bit addressing (this project's car has none), or a vendor capture of its flow-control filter |
| K-line (ISO 9141, ISO 14230), `FIVE_BAUD_INIT`, `FAST_INIT` | A vehicle or bench ECU with a K-line, plus Windows captures of the vendor DLL initialising one |
| J1850 VPW and PWM | A vehicle or bench ECU on that bus, plus captures |
| The pin-switched `*_PS` protocols, `J1962_PINS` | A vehicle wired for them (single-wire or other pin routes), plus captures |
| `PassThruSetProgrammingVoltage` | A scope or meter on the connector and an explicit electrical sign-off; it drives voltage onto a vehicle pin |
| Changing `DT_PULLUP_VALUE`, `DT_HALF_DUPLEX` | The same electrical sign-off |

## ABI, versions and threads

**ABI.** The library exports exactly the 14 `PassThru*` functions, all under the symbol version `MONGOOSE_0`, and
nothing else (a CTest enforces both). The soname is `libmongoose_j2534.so.0`. Within a soname the 14 function
signatures and the structures in `j2534.h` do not change. A new exported function would get a new version node
(`MONGOOSE_1`) and keep the soname; anything that breaks a signature or structure bumps the soname.

**Versions.** The package version is 0.1.0. Until 1.0 the API and ABI may change with the minor version, so
`find_package(mongoose-j2534 0.1)` accepts only 0.1.x. There are no releases yet.

**Threads.** Any thread may call any function. Commands to one adapter are serialised. A `PassThruReadMsgs` or
timed `PassThruWriteMsgs` blocked on a channel is woken, with an error, by `PassThruDisconnect` or
`PassThruClose` on it from another thread. The error text from `PassThruGetLastError` is **per thread**: it is
the last failure of the calling thread. Behaviour across `fork(2)` is not defined and was not tested: open the
adapter in the process that uses it.

## Safety

You use this library on a vehicle at your own risk; see the notice at the top of this file.

- Everything here that touches a vehicle should be read-only OBD-II unless you know what you are doing.
  Programming, clearing codes, security access and session control are all reachable through the API and none
  is guarded.
- Do not `SET_CONFIG` `DATA_RATE`, the sample point or jump width on a live bus.
- Do not call the adapter's firmware-update or serial-number commands. The library has no API for them, and
  none should be sent by hand.

## Testing without hardware

`mongoose-client --script-check FILE...` parses the capture scripts in `tools/scripts/` without an adapter.
`mongoose-client [serial:S] --script FILE` runs one against a real adapter, printing the same log as the
Windows harness so the two can be diffed. CTest runs 19 tests against recorded and scripted adapters; the three SocketCAN bridge tests need a `vcan0`
interface and skip without one.
