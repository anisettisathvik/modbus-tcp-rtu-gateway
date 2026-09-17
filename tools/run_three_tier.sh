#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# The full three-tier system, no hardware:
#
#   Python TCP master  ->  Modbus TCP  ->  Gateway (C++)
#                                              |
#                                          real serial
#                                              |
#                                       socat pty pair
#                                              |
#                                    Project 1 RTU slave (C)
#
# Every layer is the real implementation. The only substitution is socat
# standing in for the RS-485 transceiver pair.
# ---------------------------------------------------------------------------
set -u
cd "$(dirname "$0")/.."
PORT=${PORT:-5050}
SLAVE_DIR=${SLAVE_DIR:-$HOME/mb-rtu}

cleanup() { kill ${GW:-} ${SLAVE:-} ${SOCAT:-} 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

if [ ! -x "$SLAVE_DIR/build/host_slave" ]; then
    echo "building Project 1 host_slave..."
    ( cd "$SLAVE_DIR" && make build/host_slave >/dev/null 2>&1 )
fi

rm -f /tmp/gw_serial /tmp/slave_serial
socat pty,raw,echo=0,link=/tmp/gw_serial pty,raw,echo=0,link=/tmp/slave_serial \
      >/dev/null 2>&1 &
SOCAT=$!
sleep 2

"$SLAVE_DIR/build/host_slave" /tmp/slave_serial 17 > three_tier_slave.log 2>&1 &
SLAVE=$!
sleep 1

./build/gateway --config gateway.conf --port "$PORT" --quiet \
      > three_tier_gw.log 2>&1 &
GW=$!
sleep 1

echo "=== tier 3: TCP master -> gateway -> serial -> RTU slave ==="
python3 tools/tcp_master.py --port "$PORT"
RC=$?

echo ""
echo "--- gateway diagnostics (SIGUSR1) ---"
kill -USR1 $GW 2>/dev/null
# nudge the accept loop so it processes the signal
python3 - "$PORT" << 'PY' 2>/dev/null || true
import socket, sys
try:
    socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1).close()
except Exception:
    pass
PY
sleep 1
tail -8 three_tier_gw.log

echo ""
echo "--- RTU slave side ---"
tail -6 three_tier_slave.log
exit $RC
