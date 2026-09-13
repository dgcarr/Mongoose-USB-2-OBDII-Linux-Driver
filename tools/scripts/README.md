# Reference capture scripts

One J2534 call per line, run by `mongoose-reference.exe <dll> --script FILE --gap 2000`.
Each file is one experiment. Change exactly one argument between runs and diff the wire.

Steps: `open H`, `close H`, `version H`, `connect DEV PROTO FLAGS BAUD CHAN`,
`disconnect CHAN`, `filter CHAN TYPE PROTO FLAGS MASK PATTERN [FLOW] NAME`,
`stopfilter CHAN F`, `read CHAN COUNT TIMEOUT`, `readfor CHAN COUNT TIMEOUT SECONDS`,
`write CHAN PROTO FLAGS HEX TIMEOUT`, `getconfig CHAN P[,P...]`,
`setconfig CHAN P=V[,...]`, `vbatt DEV`, `ioctl H NAME`, `sleep MS`, `mark TEXT`.

Handles are names; a bare number is passed through verbatim so error-path scripts can
supply an invalid ID. `PassThruSetProgrammingVoltage` is not implemented on purpose.

Phase B runs on the bench. Phases C-F need the vehicle. Scripts whose name ends
`-expectfail` are expected to exit 1; the returned error code is the result.
