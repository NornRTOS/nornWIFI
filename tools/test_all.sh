#!/bin/bash
# Runs every apps/nw* application on the ESP32 board and prints one line per result;
# the radio has no emulation, so there is nothing for QEMU. Exit code 1 if anything
# failed. nw02 and nw04 need apps/nw02/nw_secrets.h (copy the example) and are skipped without it.
#
#   APPS="nw01"             subset (default: all apps/nw*)
#   ESP_PORT=/dev/ttyUSB0   serial port; skipped when absent
#   NORNOS_DIR=../nornCore
set -u
cd "$(dirname "$0")/.."
NW_DIR=$PWD
NORNOS_DIR=${NORNOS_DIR:-$(realpath "$(dirname "$0")/../../nornCore")}
ESP_PORT=${ESP_PORT-/dev/ttyUSB0}
ESP_PYTHON=${ESP_PYTHON:-$HOME/dev/tools/esp-venv/bin/python}
APPS=${APPS:-$(ls -d apps/nw* | xargs -n1 basename)}
LOGS=$NW_DIR/build/test-logs
mkdir -p "$LOGS"
failures=0; total=0

report() { total=$((total + 1)); printf '%-24s %-5s %s\n' "$1" "$2" "$3"; [ "$2" = PASS ] || [ "$2" = SKIP ] || failures=$((failures + 1)); }
clean_log() { sed 's/\x1b\[[0-9;]*m//g' | tr -d '\r'; }
mon_secs() { sed -n 's/^NW_MON_SECS *= *//p' "apps/$1/app.mk" 2>/dev/null | grep . || echo 20; }

if [ ! -e "$ESP_PORT" ]; then
    echo "esp32: $ESP_PORT not present, nothing to run"
    exit 0
fi
for app in $APPS; do
    log=$LOGS/$app-esp32.log
    if grep -q '^NW_SKIP_TEST' "apps/$app/app.mk" 2>/dev/null; then
        continue                                    # a soak or tool, not a test
    fi
    if { [ "$app" = nw02 ] || [ "$app" = nw04 ]; } && [ ! -f apps/nw02/nw_secrets.h ]; then
        report "$app esp32" SKIP "no apps/nw02/nw_secrets.h"; continue
    fi
    if ! make -s -C "$NORNOS_DIR" APP="$app" APPDIR="$NW_DIR/apps/$app" BOARD=esp32 PORT="$ESP_PORT" upload > "$log" 2>&1; then
        report "$app esp32" FAIL "build or upload failed; see $log"; continue
    fi
    "$ESP_PYTHON" "$NORNOS_DIR/tools/monitor.py" "$ESP_PORT" 115200 "$(mon_secs "$app")" 2>&1 | clean_log > "$log"
    line=$(grep -a -o "RESULT $app [A-Z]*" "$log" | tail -1)
    if [ -z "$line" ]; then
        report "$app esp32" NONE "no RESULT line; see $log"
    else
        report "$app esp32" "${line##* }" "$(grep -a 'CHECK FAILED' "$log" | head -1)"
    fi
done
echo "----"
echo "$((total - failures)) of $total passed"
[ $failures -eq 0 ]
