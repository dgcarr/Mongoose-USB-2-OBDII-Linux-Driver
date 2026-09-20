"""Load and loss test for mongoose-socketcan over the real library, driven by a simulated adapter on a pty
(tests/socketcan_load_harness.cpp):  socketcan_load_test.py BRIDGE [IFNAME]

Two things the car would otherwise have to prove: that the bridge keeps up with a busy bus without losing frames,
and that it exits when the adapter goes away. Exits 77 (CTest's skip) without a CAN interface."""
import os, re, socket, subprocess, sys, time

bridge = sys.argv[1]
interface = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("MONGOOSE_TEST_CANIF", "vcan0")
frames, rate = 3000, 2450  # the rate measured on the car
try:
    listener = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    listener.bind((interface,))          # bound before the bridge starts, so no frame is missed
except OSError as error:
    print(f"SKIP: no CAN interface '{interface}' ({error})")
    sys.exit(77)

def run(**settings):
    environment = dict(os.environ, **{f"MONGOOSE_LOAD_{k}": str(v) for k, v in settings.items()})
    return subprocess.Popen([bridge, interface], env=environment, stderr=subprocess.PIPE, text=True)

listener.settimeout(10)
process = run(RATE=rate, TOTAL=frames)
received, started = 0, None
try:
    while received < frames:
        listener.recv(16)
        started = started or time.time()
        received += 1
except socket.timeout:
    pass
elapsed = time.time() - started if started else 0
process.terminate()
summary = process.stderr.read()
assert process.wait(10) == 0, f"bridge exited {process.returncode}\n{summary}"
assert received == frames, f"received {received} of {frames} frames\n{summary}"
assert re.search(r"from vehicle %d, to vehicle 0, refused 0, adapter overflows 0, interface drops 0" % frames,
                 summary), summary
print(f"PASS: {received} frames, no loss, {received / elapsed:.0f}/s")

# The adapter going away (here the pty closing, as an unplug does) must stop the bridge, not hang it.
process = run(RATE=rate, TOTAL=10 ** 6, HANGUP=600)
try:
    assert process.wait(15) == 1, f"bridge exited {process.returncode} after the adapter went away"
except subprocess.TimeoutExpired:
    process.kill()
    raise AssertionError("bridge did not exit after the adapter went away")
assert "adapter disconnected" in process.stderr.read()
print("PASS: stopped when the adapter went away")
