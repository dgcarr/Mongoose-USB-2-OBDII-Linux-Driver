# MongoosePro JLR Linux driver

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

This is an experimental native C++20 J2534 library for the MongoosePro JLR USB adapter (`18e1:0104`). It talks to the hardware as an ordinary `cdc_acm` serial device — no vendor SDK, no Windows DLL — reconstructed from the closed driver with Ghidra, then checked against the wire.

Start with [docs/USING.md](docs/USING.md) to use it, [docs/VALIDATION.md](docs/VALIDATION.md) for what has been tested and how, and [plan.md](plan.md) for what is left. The spirit is research first and claims last. Linux can already discover the adapter, open it, stand up a CAN channel and speak the framed command set, but nothing here pretends to be a finished diagnostic stack. It has been run on a live car for an hour of traffic and a hundred lifecycle cycles, but only over CAN, and large parts of J2534 are still ahead.
