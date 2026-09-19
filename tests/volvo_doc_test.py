"""Runs the Python examples in docs/VOLVO.md through mongoose-socketcan built against tests/fake_j2534.c, so the guide
cannot drift from what works:  volvo_doc_test.py BRIDGE GUIDE [IFNAME]
Exits 77 (CTest's skip) without a CAN interface or the kernel's ISO-TP sockets. The udsoncan example also needs
`pip install udsoncan can-isotp`; without those it is skipped and the standard-library one still runs."""
import contextlib, io, os, re, socket, subprocess, sys, time

bridge, guide = sys.argv[1], sys.argv[2]
interface = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("MONGOOSE_TEST_CANIF", "vcan0")
try:
    probe = socket.socket(socket.AF_CAN, socket.SOCK_DGRAM, socket.CAN_ISOTP)
    probe.bind((interface, 0x7E8, 0x7E0))
    probe.close()
except OSError as error:
    print(f"SKIP: no CAN interface '{interface}' with ISO-TP sockets ({error})")
    sys.exit(77)

with open(guide, encoding="utf-8") as text:
    blocks = [b.replace('"mongoose0"', repr(interface)) for b in re.findall(r"```python\n(.*?)```", text.read(), re.S)]
assert len(blocks) == 2, f"expected the standard-library and udsoncan examples, found {len(blocks)} blocks"

process = subprocess.Popen([bridge, "--transmit", interface], stderr=subprocess.DEVNULL)
try:
    time.sleep(0.5)
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        exec(blocks[0], {})
    assert output.getvalue() == ("rpm 750.0\ncoolant 83 C\nVIN YV1TESTVIN0000000\n['P0171', 'C0420']\n"), output.getvalue()
    try:
        import isotp, udsoncan  # noqa: F401
    except ImportError:
        print("note: udsoncan or can-isotp not installed; the udsoncan example was skipped")
    else:
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            exec(blocks[1], {})
        assert output.getvalue() == "YV1TESTVIN0000000\n", output.getvalue()
finally:
    process.terminate()
    assert process.wait(10) == 0
print("PASS")
