#!/bin/sh
# Test the dependency allowlist separately from native binary inspection.
set -eu
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir "$work/bin"
cat > "$work/bin/uname" <<'SH'
#!/bin/sh
printf 'Linux\n'
SH
cat > "$work/bin/readelf" <<'SH'
#!/bin/sh
if [ "$1" = -d ]; then
    for name in $GM_TEST_NEEDED; do printf ' (NEEDED) Shared library: [%s]\n' "$name"; done
elif [ "$GM_TEST_INTERP" = 1 ]; then
    printf ' INTERP\n'
fi
SH
chmod +x "$work/bin/uname" "$work/bin/readelf"
export PATH="$work/bin:$PATH" GM_TEST_INTERP=0 GM_TEST_NEEDED='libc.so.6 libm.so.6 ld-linux-aarch64.so.1'
sh tools/check-linkage.sh ignored system > "$work/log"
GM_TEST_NEEDED='libc.so.6 ld-linux-x86-64.so.2' sh tools/check-linkage.sh ignored system > "$work/log"
if GM_TEST_NEEDED='libcrypto.so.3' sh tools/check-linkage.sh ignored system > "$work/log" 2>&1; then exit 1; fi
if sh tools/check-linkage.sh ignored static > "$work/log" 2>&1; then exit 1; fi
if GM_TEST_NEEDED='' GM_TEST_INTERP=1 sh tools/check-linkage.sh ignored static > "$work/log" 2>&1; then exit 1; fi
GM_TEST_NEEDED='' sh tools/check-linkage.sh ignored static > "$work/log"
printf 'PASS: system loaders allowed, external dependencies and dynamic static-profile binaries refused\n'
