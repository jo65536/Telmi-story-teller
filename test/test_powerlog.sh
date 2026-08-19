#!/bin/sh
set -eu

repo_root=$(CDPATH=; cd -- "$(dirname -- "$0")/.." && pwd)
logger=$repo_root/static/build/.tmp_update/script/powerlog.sh
test_root=$(mktemp -d "${TMPDIR:-/tmp}/telmi-powerlog-test.XXXXXX")
logger_pid=

cleanup() {
    if [ -n "$logger_pid" ] && kill -0 "$logger_pid" 2> /dev/null; then
        kill "$logger_pid"
        wait "$logger_pid" 2> /dev/null || true
    fi
    rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM

saves=$test_root/Saves
ram=$test_root/ram
mkdir -p "$saves" "$ram"
printf 'patched\n' > "$saves/.powerTelemetry"
printf 'miyoo-a\n' > "$saves/.powerDevice"
printf '61\n' > "$ram/percBat"
printf '{"battery":61, "voltage":3901, "charging":0}\n' > "$ram/.axp_result"
printf '0\n' > "$ram/telmi-display-state"
printf '354\n' > "$ram/deviceModel"
printf '640x480\n' > "$ram/screen_resolution"

TELMI_SAVESDIR=$saves TELMI_RAMDIR=$ram TELMI_SYSDIR=$test_root busybox sh "$logger" &
logger_pid=$!

attempt=0
while [ ! -s "$ram/telmi-power.csv" ] && [ "$attempt" -lt 100 ]; do
    sleep 0.05
    attempt=$((attempt + 1))
done
[ -s "$ram/telmi-power.csv" ]

kill "$logger_pid"
wait "$logger_pid"
logger_pid=

set -- "$saves"/Diagnostics/power/power-miyoo-a-patched-*.csv
[ "$#" -eq 1 ]
persisted=$1
[ -f "$persisted" ]
cmp "$ram/telmi-power.csv" "$persisted"

awk -F, '
    /^#/ { next }
    $1 == "epoch" {
        if (NF != 24) exit 1
        header = 1
        next
    }
    {
        if (!header) exit 1
        if (NF != 24) exit 1
        rows++
    }
    END {
        if (!header || rows < 2) exit 1
    }
' "$persisted"

grep -q '^# sample_interval_s=60$' "$persisted"
grep -q ',61,3901,0,0,' "$persisted"

bad_saves=$test_root/bad-Saves
bad_ram=$test_root/bad-ram
mkdir -p "$bad_saves" "$bad_ram"
printf 'patched\n' > "$bad_saves/.powerTelemetry"
ln -s /proc "$bad_saves/Diagnostics"
TELMI_SAVESDIR=$bad_saves TELMI_RAMDIR=$bad_ram TELMI_SYSDIR=$test_root busybox sh "$logger" \
    > /dev/null 2>&1 &
logger_pid=$!
attempt=0
while [ ! -s "$bad_ram/telmi-power.csv" ] && [ "$attempt" -lt 100 ]; do
    sleep 0.05
    attempt=$((attempt + 1))
done
[ -s "$bad_ram/telmi-power.csv" ]
kill "$logger_pid"
set +e
wait "$logger_pid"
flush_status=$?
set -e
logger_pid=
[ "$flush_status" -ne 0 ]
[ -s "$bad_saves/telmi-power-flush-error.txt" ]

printf 'powerlog test: ok\n'
