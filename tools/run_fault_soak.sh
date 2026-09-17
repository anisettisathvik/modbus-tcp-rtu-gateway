#!/usr/bin/env bash
# Soak the gateway with deterministic bus faults injected.
set -u
cd "$(dirname "$0")/.."
PORT=${PORT:-5040}
./build/gateway --port $PORT --units 17 --drop 0.15 --corrupt 0.15 --seed 42 --quiet \
    > fault_gw.log 2>&1 &
GW=$!
sleep 1
python3 tools/tcp_master.py --port $PORT --clients "${1:-20}" --txns "${2:-300}"
RC=$?
kill -INT $GW 2>/dev/null
wait $GW 2>/dev/null
echo "--- gateway stats ---"
tail -3 fault_gw.log
exit $RC
