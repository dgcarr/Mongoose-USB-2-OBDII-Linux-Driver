# Linux J2534 driver for MongoosePro JLR (OBD-II / SocketCAN)

[![CI](https://github.com/dgcarr/Mongoose-USB-2-OBDII-Linux-Driver/actions/workflows/ci.yml/badge.svg)](https://github.com/dgcarr/Mongoose-USB-2-OBDII-Linux-Driver/actions/workflows/ci.yml)
[![License: LGPL-2.1-or-later](https://img.shields.io/badge/License-LGPL--2.1--or--later-blue.svg)](LICENSE)

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

This is an unofficial Linux driver for the Drew Tech **MongoosePro JLR** USB adapter (`18e1:0104`): an
experimental native C++20 J2534 PassThru library that talks to the hardware as an ordinary `cdc_acm`
serial device — no vendor SDK, no Windows DLL — reconstructed from the closed driver with Ghidra, then
checked against the wire.

Use it for Linux OBD-II diagnostics on a vehicle whose diagnostic CAN bus is on pins 6 and 14 (Volvo,
Jaguar, Land Rover and most cars sold since about 2008), or run `mongoose-socketcan` so the adapter is
an ordinary SocketCAN interface for can-utils, Wireshark, python-can and udsoncan.

## Who this is for

- Linux users with a MongoosePro JLR who want OBD-II / CAN access without Windows or the vendor SDK
- Anyone who wants SocketCAN rather than a J2534 client (`mongoose-socketcan`)
- Developers writing J2534 programs against this library — see [docs/USING.md](docs/USING.md)

It is not a vendor product, not J2534-certified, and not a driver for ELM327 or other adapters.

## Quick start

You need a **MongoosePro JLR** (`18e1:0104`) and a car with diagnostic CAN on OBD-II pins 6 and 14.

**To talk to your car, start with [docs/VOLVO.md](docs/VOLVO.md)**: clone, build, and read live data,
trouble codes and the VIN from Linux in a few minutes.

```sh
git clone https://github.com/dgcarr/Mongoose-USB-2-OBDII-Linux-Driver.git
cd Mongoose-USB-2-OBDII-Linux-Driver
cmake -S . -B build && cmake --build build
```

Then follow that guide for serial-port access, a first OBD-II request, and the SocketCAN bridge.
`mongoose-socketcan` makes the adapter an ordinary Linux CAN interface, so can-utils, Wireshark,
python-can and udsoncan work with it unchanged.

## What works, and what does not

Raw CAN and 11-bit ISO15765 work and have been run on a live car for hours of traffic and hundreds of
lifecycle cycles. K-line, J1850, 29-bit ISO15765 and the pin-switched protocols are not supported,
because nothing here could test them.

[docs/VALIDATION.md](docs/VALIDATION.md) records what has been tested and how, and [plan.md](plan.md)
what is left. The spirit is research first and claims last.

## Documentation

- [docs/VOLVO.md](docs/VOLVO.md) — from clone to live data on the car (Volvo notes included)
- [docs/USING.md](docs/USING.md) — writing a J2534 program against the library
- [docs/VALIDATION.md](docs/VALIDATION.md) — tested vs untested
- [PROTOCOL.md](PROTOCOL.md) — USB / adapter protocol
- [NOTES.md](NOTES.md) — research status

## Licence

The driver's own code and documentation are licensed under the **GNU Lesser General Public License,
version 2.1 or (at your option) any later version**: see [LICENSE](LICENSE). Programs that load the library
at run time, as J2534 clients do, need not themselves be licensed under the LGPL.

The LGPL covers only what was written for this project. The vendor's Windows driver files (`vendor/`) and the
Ghidra decompilations and disassemblies made from them (`analysis/decompiled/`, `analysis/disassembly/`) belong
to their owners. They are not tracked in this repository, are ignored by git, and are not licensed by this
project or included in any package it builds. Notes elsewhere that cite those paths refer to a local research
copy; a fresh clone does not have it, and `analysis/REPRODUCE.md` says how to regenerate it from a driver you have.
