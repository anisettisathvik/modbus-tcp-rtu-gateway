#!/usr/bin/env bash
# Capture live Modbus TCP and decode it with the real Wireshark dissector.
set -u
cd "$(dirname "$0")/.."
mkdir -p captures
PORT=${PORT:-5020}
./build/gateway --port $PORT --units 17,34,51 --quiet > capture_gw.log 2>&1 &
GW=$!
sleep 1
tcpdump -i lo -w captures/modbus_tcp.pcap "tcp port $PORT" > /dev/null 2>&1 &
TD=$!
sleep 1
python3 tools/tcp_master.py --port $PORT
sleep 1
kill $TD 2>/dev/null; wait $TD 2>/dev/null
kill -INT $GW 2>/dev/null; wait $GW 2>/dev/null

echo ""
echo "=== Wireshark Modbus dissector output ==="
tshark -r captures/modbus_tcp.pcap -d tcp.port==$PORT,mbtcp -Y mbtcp \
  -T fields -e frame.number -e mbtcp.trans_id -e mbtcp.unit_id \
  -e modbus.func_code -e modbus.exception_code -E header=y 2>/dev/null
