#!/bin/bash
# The stability gate over the radio: flashes apps/nw05 and, for SECONDS (default 86400),
# records the board's log while the host pings it with 1400 byte packets five times a
# second; tools/tcp_sink.py of nornNET must be listening on port 17010 of the host named
# in apps/nw02/nw_secrets.h. Summary at the end: rounds, failures, link losses, heap and
# buffer drift, ping loss. Logs in build/soak/.
#
#   tools/soak_wifi.sh [SECONDS] [BOARD_IP]
set -u
cd "$(dirname "$0")/.."
SECS=${1:-86400}
NORNOS_DIR=${NORNOS_DIR:-$(realpath "$(dirname "$0")/../../nornCore")}
ESP_PORT=${ESP_PORT:-/dev/ttyUSB0}
ESP_PYTHON=${ESP_PYTHON:-$HOME/dev/tools/esp-venv/bin/python}
mkdir -p build/soak
make -s nw05-upload > build/soak/upload.log 2>&1 || { echo "upload failed"; tail -5 build/soak/upload.log; exit 1; }
( "$ESP_PYTHON" -u "$NORNOS_DIR/tools/monitor.py" "$ESP_PORT" 115200 "$((SECS + 30))" 2>&1 | sed -u 's/\x1b\[[0-9;]*m//g; s/\r//g' > build/soak/board.log ) &
MON=$!
IP=${2:-}
if [ -z "$IP" ]; then
    for i in $(seq 1 60); do
        IP=$(grep -a -o "nw05: address [0-9.]*" build/soak/board.log 2>/dev/null | head -1 | awk '{print $3}')
        [ -n "$IP" ] && break
        sleep 1
    done
fi
echo "board at ${IP:-unknown}, soaking for $SECS s"
if [ -n "$IP" ]; then
    ping -D -s 1400 -i 0.2 -w "$((SECS - 70))" "$IP" > build/soak/ping.log 2>&1
fi
wait $MON
echo "=== board"
grep -a -c "nw05: round" build/soak/board.log | sed 's/^/rounds: /'
grep -a "nw05: round" build/soak/board.log | tail -1
grep -a -c "FAIL\|transfer failed" build/soak/board.log | sed 's/^/failed rounds: /'
grep -a -c "link down" build/soak/board.log | sed 's/^/link downs: /'
grep -a -c "dhcp" build/soak/board.log | sed 's/^/dhcp lines: /'
grep -a -c "rst:0x\|FATAL\|ASSERT" build/soak/board.log | sed 's/^/resets or faults: /'
echo "=== host pings"
grep -E "transmitted|rtt" build/soak/ping.log
