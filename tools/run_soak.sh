#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/.."
PORT=${PORT:-5041}
./build/gateway --port $PORT --units 17,34,51 --quiet > soak_gw.log 2>&1 &
GW=$!
sleep 1
python3 tools/tcp_master.py --port $PORT
python3 tools/tcp_master.py --port $PORT --clients "${1:-100}" --txns "${2:-200}"
RC=$?
kill -INT $GW 2>/dev/null; wait $GW 2>/dev/null
echo "--- gateway stats ---"; tail -3 soak_gw.log
exit $RC
