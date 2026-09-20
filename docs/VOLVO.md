# Talking to your car from Linux (Volvo and others)

> **Use at your own risk.** Read the notice in the [README](../README.md) first. In short: this is an unofficial,
> reverse-engineered driver, tested on **one** car (a 2017 Volvo XC60 D5 AWD) with one adapter. Sending the wrong
> thing to a car can set fault codes, disable systems or drain the battery. Everything below is read-only unless
> it says otherwise, and the bridge is listen-only unless you pass `--transmit`.

This page takes you from a fresh clone to reading data from your car with ordinary Linux software. You need a
**MongoosePro JLR** adapter (USB `18e1:0104`) and a car whose diagnostic CAN bus is on OBD-II pins 6 and 14. That
covers most cars sold since about 2008. The adapter was made for Jaguar Land Rover, but the driver only speaks
plain CAN and ISO-TP, so it is not tied to one make.

## 1. Build it

```sh
# Debian/Ubuntu: sudo apt install build-essential cmake git
# Fedora:        sudo dnf install gcc-c++ cmake git
# Arch:          sudo pacman -S base-devel cmake git
git clone https://github.com/dgcarr/Mongoose-USB-2-OBDII-Linux-Driver.git
cd Mongoose-USB-2-OBDII-Linux-Driver
cmake -S . -B build && cmake --build build
ctest --test-dir build          # all pass; the SocketCAN test skips until step 4 creates a CAN interface
```

libusb is optional and not needed. Everything below runs straight from `build/`. To install instead:

```sh
sudo cmake --install build && sudo ldconfig    # without ldconfig the programs cannot find the library
```

That puts the library, the programs, the man pages, the udev rule and a systemd unit under `/usr/local`.

## 2. Let your user open the adapter

The adapter is an ordinary USB serial port (`/dev/ttyACM0`). Either add yourself to the group that owns serial ports
(`dialout` on Debian, Ubuntu and Fedora; `uucp` on Arch) and log out and back in, or install the udev rule, which
also stops ModemManager from sending the adapter modem commands:

```sh
sudo cp packaging/60-mongoose-j2534.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger     # then unplug and replug the adapter
build/mongoose-diag --list                                # expect: tty /dev/ttyACM0 serial=... access=ok
```

## 3. First contact with the car

Plug the adapter into the OBD port under the dashboard and switch the ignition on, or start the engine. Then:

```sh
build/mongoose-example-obd
```

It sends one read-only OBD-II request (mode 01 PID 00, "which values do you support?") and prints each engine or
gearbox computer that answers. On the XC60 two do:

```
reply from 0x07e8: 41 00 ...
reply from 0x07e9: 41 00 ...
```

`PassThruWriteMsgs failed ... did not confirm transmission` means the adapter saw no CAN bus. Check that the
ignition is on and the plug is fully in.

## 4. Make the adapter a Linux CAN interface

`mongoose-socketcan` bridges the adapter to a SocketCAN interface. Once it runs, any SocketCAN program can use the
car's bus: `candump` and `cansniffer` from can-utils, Wireshark, SavvyCAN, python-can, udsoncan, and the kernel's
own ISO-TP sockets. Create the interface once per boot (it is a virtual one, `vcan`; the bridge fills it):

```sh
sudo modprobe -a vcan can-isotp
sudo ip link add dev mongoose0 type vcan
sudo ip link set mongoose0 up
```

If you installed in step 1, systemd can do all of that for you, at boot or on demand, and it creates the interface
itself:

```sh
sudo systemctl start mongoose-socketcan@mongoose0     # add `enable` to have it start at every boot
systemctl status mongoose-socketcan@mongoose0
```

That service is listen-only. To let programs transmit, `sudo systemctl edit mongoose-socketcan@mongoose0` and give
it an `ExecStart` with `--transmit`, as the unit's own comments show. The rest of this page assumes you are running
the bridge by hand:

Then run the bridge, and leave it running while you use the car:

```sh
build/mongoose-socketcan --stats 10 mongoose0             # listen only: nothing reaches the car
```

In another terminal, watch the traffic (install can-utils first: `sudo apt install can-utils`):

```sh
candump -ta mongoose0          # every frame
cansniffer -c mongoose0        # one line per ID, changing bytes highlighted
```

Wireshark (`tshark -i mongoose0`), SavvyCAN and python-can read the same interface. One quirk: because the bridge
writes the car's frames into a virtual interface, they are *locally generated* as far as the kernel is concerned, so
python-can reports them as `Tx` and `msg.is_rx == False`. Do not filter on that, or you will discard everything the
car said.

To **ask** the car things, restart the bridge with `--transmit`. From then on, any frame a program writes to
`mongoose0` goes onto the car's bus, so only run programs you trust:

```sh
build/mongoose-socketcan --transmit --stats 10 mongoose0
```

Options: `--bitrate` (default 500000, the rate of every car tested so far; a wrong rate on a live bus is not
harmless), `--29bit` (open the channel for 29-bit identifiers; see below), `--device serial:...` (pick one of
several adapters). Stop the bridge with Ctrl-C. Only one program can hold the adapter at a time, so stop the bridge
before running `mongoose-client` or the example.

## 5. Asking questions with Python

The kernel does the ISO-TP segmenting and flow control, so a request is one `send` and a reply of any length is one
`recv`. This needs nothing beyond Python's standard library:

```python
import socket, struct

def ecu(request_id=0x7E0, reply_id=0x7E8, interface="mongoose0"):
    s = socket.socket(socket.AF_CAN, socket.SOCK_DGRAM, socket.CAN_ISOTP)
    # SOL_CAN_ISOTP, CAN_ISOTP_OPTS: pad every frame to 8 bytes with 0x55. The ECUs tested ignore short frames.
    s.setsockopt(106, 1, struct.pack("=IIBBBB", 0x4, 0, 0, 0x55, 0, 0))
    s.bind((interface, reply_id, request_id))
    s.settimeout(2)
    return s

engine = ecu()                                     # engine computer: requests to 0x7E0, replies from 0x7E8

engine.send(bytes([0x01, 0x0C]))                   # OBD mode 01 PID 0C: engine speed
r = engine.recv(4095)                              # 41 0C A B
print("rpm", (r[2] * 256 + r[3]) / 4)

engine.send(bytes([0x01, 0x05]))                   # coolant temperature
print("coolant", engine.recv(4095)[2] - 40, "C")

engine.send(bytes([0x09, 0x02]))                   # VIN (a multi-frame reply, reassembled for you)
print("VIN", engine.recv(4095)[3:].decode())

engine.send(bytes([0x03]))                         # stored trouble codes: 43 N, then two bytes per code
r = engine.recv(4095)
codes = r[2:] if len(r) % 2 == 0 else r[1:]
print(["PCBU"[b >> 6] + f"{(b >> 4) & 3}{b & 15:X}{c:02X}" for b, c in zip(codes[::2], codes[1::2]) if b or c])
```

For UDS, the service Volvo's newer cars use for everything beyond OBD-II, udsoncan works over the same interface
(`pip install udsoncan can-isotp`):

```python
import isotp, udsoncan
from udsoncan.client import Client
from udsoncan.connections import IsoTPSocketConnection

address = isotp.Address(isotp.AddressingMode.Normal_11bits, txid=0x7E0, rxid=0x7E8)
connection = IsoTPSocketConnection("mongoose0", address, tpsock=isotp.socket(timeout=2))
connection.tpsock.set_opts(txpad=0x55)
config = {"data_identifiers": {0xF190: udsoncan.AsciiCodec(17)}}
with Client(connection, request_timeout=2, config=config) as client:
    print(client.read_data_by_identifier(0xF190).service_data.values[0xF190])      # the VIN
```

Both snippets were run through the bridge against a simulated ECU (the `socketcan_bridge` test does the same
multi-frame VIN read). Through the J2534 API, the XC60 answered the same kinds of request: mode 01, the mode 09 VIN,
modes 03 and 07 (both `43 00`/`47 00`, no codes stored) and UDS `22 F190`. See [VALIDATION.md](VALIDATION.md).
The bridge itself has not yet been run on the car.

### Staying read-only

These are safe to send: OBD-II modes `01` (live data), `02` (freeze frame), `03`, `07` and `0A` (stored, pending
and permanent codes), `09` (VIN and calibration IDs), and the UDS services `22` (read a data identifier) and `19`
(read trouble codes). **Do not** send, unless you know exactly what it does to your car: mode `04` (clears codes and
readiness monitors), UDS `10` (session control), `27` (security access), `2E` (write), `31` (routines), `11`
(reset), `14` (clear codes), `34`-`37` (programming), or anything a forum post calls "coding".

## Volvo notes

Only the 2017 XC60 has been tried. The first point below is what it showed; the others are general guidance to
help you judge your own car, not tested fact.

- **Engine and gearbox.** On the XC60, `0x7E0`/`0x7E8` (`ECM-EngineControl`) and `0x7E1`/`0x7E9`
  (`TCM-TransmisCtrl`) answer OBD-II, both functionally (to `0x7DF`) and physically, and the engine ECU answers UDS
  `22` (`F190` VIN, `F18C` serial). No other ECU answered in that range. The car's other modules use their own
  addresses behind the central gateway, and they were not probed.
- **One bus only.** The driver reaches the high-speed CAN on pins 6 and 14. Several Volvo generations also put a
  second, slower CAN (125 kbit) on pins 3 and 11. Reaching it needs the adapter's pin switching (the `*_PS`
  protocols), which this driver does not support yet, so modules on that bus are out of reach.
- **Older Volvos and 29-bit IDs.** Earlier platforms talk to most modules with Volvo's own diagnostics on 29-bit CAN
  identifiers. The bridge passes 29-bit frames both ways, and transmit takes either width. For receive, start the
  bridge with `--29bit`. Whether one channel receives both widths at once has not been tested, and neither has any
  29-bit Volvo.
- **VIDA** and other Windows diagnostic programs do not run on this driver. It is a Linux library and bridge, not a
  Windows J2534 DLL.

## When something is wrong

| Symptom | Cause |
|---|---|
| `no interface 'mongoose0'` | Step 4's `ip link` commands were not run since the last boot |
| `PassThruOpen failed ... Device or resource busy` | Another program (a second bridge, `mongoose-client`) holds the adapter |
| `access=denied` from `mongoose-diag --list` | Step 2 |
| The bridge counts `from vehicle 0` | Ignition off, plug not seated, or the wrong `--bitrate` |
| Requests get no reply | The bridge is listen-only (it counts them as `refused`): restart it with `--transmit`. Or padding is off: set it as in the snippets |
| `adapter overflows` rises | Frames were lost because the host fell behind. Rare; say so in an issue with the bus rate |
| The bridge exits with `ERR_DEVICE_NOT_CONNECTED` | The adapter was unplugged. Replug it and start the bridge again |
| python-can shows every frame as `Tx` | Expected on a virtual interface; see the note in step 4 |
| An installed program says `cannot open shared object file` | `sudo ldconfig` after installing |

What has been tested, and how, is in [VALIDATION.md](VALIDATION.md). The J2534 API for writing your own programs
is in [USING.md](USING.md).
