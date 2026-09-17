#!/usr/bin/env bash
# Prove the gateway survives the serial device disappearing and returning.
# On a plant floor this is someone unplugging a USB-RS485 adapter.
set -u
cd "$(dirname "$0")/.."
PORT=${PORT:-5060}
SLAVE_DIR=${SLAVE_DIR:-$HOME/mb-rtu}

cleanup() { kill ${GW:-} ${SLAVE:-} ${SOCAT:-} 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

rm -f /tmp/rc_gw /tmp/rc_slave
socat pty,raw,echo=0,link=/tmp/rc_gw pty,raw,echo=0,link=/tmp/rc_slave >/dev/null 2>&1 &
SOCAT=$!; sleep 2
"$SLAVE_DIR/build/host_slave" /tmp/rc_slave 17 >/dev/null 2>&1 &
SLAVE=$!; sleep 1
./build/gateway --port $PORT --serial /tmp/rc_gw --quiet > reconnect_gw.log 2>&1 &
GW=$!; sleep 1

python3 - "$PORT" << 'PY'
import socket, struct, sys, time, subprocess, os
port = int(sys.argv[1])
def frame(txn, unit, pdu): return struct.pack(">HHHB", txn, 0, len(pdu)+1, unit) + pdu
def read_hold(txn, unit, start, qty): return frame(txn, unit, struct.pack(">BHH", 3, start, qty))
def recv(s):
    h = s.recv(6)
    if len(h) < 6: return None
    _,_,l = struct.unpack(">HHH", h)
    return h + s.recv(l)

s = socket.create_connection(("127.0.0.1", port), timeout=5)
ok_before = ok_during = ok_after = 0

for i in range(5):
    s.sendall(read_hold(i, 17, 0, 1)); r = recv(s)
    if r and not (r[7] & 0x80): ok_before += 1

print(f"  before unplug .......... {ok_before}/5 normal responses")

# "unplug": kill socat, destroying the pty pair
subprocess.run(["pkill", "-f", "link=/tmp/rc_gw"], capture_output=True)
time.sleep(1)

for i in range(5):
    s.sendall(read_hold(100+i, 17, 0, 1)); r = recv(s)
    if r and (r[7] & 0x80) and r[8] == 0x0B: ok_during += 1

print(f"  while unplugged ........ {ok_during}/5 exception 0x0B (not silence)")

# "replug": recreate the pair and the slave
subprocess.Popen(["socat", "pty,raw,echo=0,link=/tmp/rc_gw",
                  "pty,raw,echo=0,link=/tmp/rc_slave"],
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)
subprocess.Popen([os.environ.get("SLAVE_DIR", os.path.expanduser("~/mb-rtu")) + "/build/host_slave",
                  "/tmp/rc_slave", "17"],
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)

for i in range(8):
    s.sendall(read_hold(200+i, 17, 0, 1)); r = recv(s)
    if r and not (r[7] & 0x80): ok_after += 1
    time.sleep(0.3)

print(f"  after replug ........... {ok_after}/8 normal responses")
s.close()

if ok_before == 5 and ok_during == 5 and ok_after >= 5:
    print("\n  PASS  survived device loss and reconnected automatically")
    sys.exit(0)
print("\n  FAIL")
sys.exit(1)
PY
RC=$?
echo ""; echo "--- gateway bus counters ---"
kill -USR1 $GW 2>/dev/null
python3 -c "import socket;socket.create_connection(('127.0.0.1',$PORT),timeout=1).close()" 2>/dev/null
sleep 1; grep -A3 "diagnostics" reconnect_gw.log | tail -4
exit $RC
