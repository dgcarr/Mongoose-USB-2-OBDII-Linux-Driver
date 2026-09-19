# Reference capture scripts

One J2534 call per line, run by `mongoose-reference.exe <dll> --script FILE --gap 2000`.
Each file is one experiment. Change exactly one argument between runs and diff the wire.

Steps: `open H`, `close H`, `version H`, `connect DEV PROTO FLAGS BAUD CHAN`,
`disconnect CHAN`, `filter CHAN TYPE PROTO FLAGS MASK PATTERN [FLOW] NAME`,
`stopfilter CHAN F`, `read CHAN COUNT TIMEOUT`, `readfor CHAN COUNT TIMEOUT SECONDS`,
`write CHAN PROTO FLAGS HEX TIMEOUT`, `periodic CHAN PROTO FLAGS HEX INTERVAL_MS NAME`,
`stopperiodic CHAN NAME`, `getconfig CHAN P[,P...]`, `setconfig CHAN P=V[,...]`, `vbatt DEV`,
`ioctl H NAME`, `sleep MS`, `mark TEXT`.

The same files run on both stacks: `mongoose-reference.exe <dll> --script FILE` on Windows and
`mongoose-client [serial:SERIAL] --script FILE` on Linux (with `--gap MS` on either).
`mongoose-client --script-check FILE...` parses without touching an adapter, and the `script_syntax`
CTest runs it over every file here. Both interpreters print the same log lines, so a run on each can be
diffed after stripping the `utc_100ns=` timestamps.

Never `setconfig` an ID in `0xC002`-`0xC00A` (`NON_VOLATILE_STORE_*`): those write non-volatile memory.

Handles are names; a bare number is passed through verbatim so error-path scripts can
supply an invalid ID. `PassThruSetProgrammingVoltage` is not implemented on purpose.

Phase B runs on the bench. Phases C-F need the vehicle. Scripts whose name ends
`-expectfail` are expected to exit 1; the returned error code is the result.
