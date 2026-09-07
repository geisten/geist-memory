#!/bin/sh
# Linux-only real-model acceptance. Each fault runs in a fresh process.
set -eu
binary=$1
logs=$2
mkdir -p "$logs"
run() {
    name=$1; shift
    if ! timeout 180 "$binary" "$@" > "$logs/$name.log" 2>&1; then
        cat "$logs/$name.log" >&2
        echo "FAIL: $name" >&2
        exit 1
    fi
}
run baseline none -1 0
# Fixed prepared 0.6B/native-NEON workload: leave headroom above the measured
# ~1.00 GB while rejecting the former ~1.46 GB unbounded model setup.
awk '
/^phase=/ { for (i=1; i<=NF; i++) if ($i ~ /^peak_bytes=/) {
    sub(/^peak_bytes=/,"",$i); if (($i + 0) > 1153433600) bad=1; seen++
} }
END { if (seen != 4 || bad) exit 1 }
' "$logs/baseline.log" || { echo "FAIL: requested peak exceeds 1100 MiB" >&2; exit 1; }
run large none -1 8388608
count() {
    awk -v phase="phase=$1" '$1 == phase { for (i=1; i<=NF; i++) if ($i ~ /^eligible=/) {sub(/^eligible=/,"",$i); print $i} }' "$2"
}
# Every allocation in the measured mutation/search paths (bounded nightly work).
for phase in remember recall compact; do
    n=$(count "$phase" "$logs/baseline.log")
    test "$n" -gt 0 && test "$n" -le 64
    i=0
    while test "$i" -lt "$n"; do
        run "$phase-$i" "$phase" "$i" 0
        i=$((i + 1))
    done
done
# All >=8-MiB startup allocations; sample smaller startup allocations separately.
n=$(count open "$logs/large.log")
test "$n" -gt 0 && test "$n" -le 64
i=0
while test "$i" -lt "$n"; do
    run "open-large-$i" open "$i" 8388608
    i=$((i + 1))
done
n=$(count open "$logs/baseline.log")
test "$n" -ge 16
# First 16, seven evenly spaced interior sites, and the last startup allocation.
i=0
while test "$i" -lt 16; do
    run "open-$i" open "$i" 0
    i=$((i + 1))
done
i=1
while test "$i" -lt 8; do
    at=$((n * i / 8))
    run "open-sample-$i" open "$at" 0
    i=$((i + 1))
done
run open-last open "$((n - 1))" 0
printf 'PASS: real-model mutation allocation sweep, all large startup allocations and startup samples\n'
