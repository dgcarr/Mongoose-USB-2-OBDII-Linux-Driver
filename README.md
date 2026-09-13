# MongoosePro JLR Linux driver

This is an experimental native C++20 J2534 library for the MongoosePro JLR USB adapter (`18e1:0104`). It talks to the hardware as an ordinary `cdc_acm` serial device — no vendor SDK, no Windows DLL — reconstructed from the closed driver with Ghidra, then checked against the wire.

The spirit is research first and claims last. Linux can already discover the adapter, open it, stand up a CAN channel and speak the framed command set, but nothing here pretends to be a finished diagnostic stack: most of it has only ever run on a bench with no bus, and vehicle work is still ahead.
