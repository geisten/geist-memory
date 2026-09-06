#!/bin/sh
set -eu
binary=$1
profile=$2
case $(uname -s) in
    Darwin)
        deps=$(otool -L "$binary" | sed '1d' | awk '{print $1}')
        printf '%s\n' "$deps"
        if printf '%s\n' "$deps" | awk '!/^\/usr\/lib\// && !/^\/System\/Library\// {bad=1} END {exit !bad}'; then
            echo 'Unexpected non-system dynamic dependency' >&2; exit 1
        fi
        ;;
    Linux)
        readelf -d "$binary"
        if [ "$profile" = static ]; then
            if readelf -l "$binary" | grep -q INTERP || readelf -d "$binary" | grep -q NEEDED; then
                echo 'Static profile contains dynamic dependencies' >&2; exit 1
            fi
        else
            deps=$(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p')
            for dep in $deps; do
                case "$dep" in
                    libc.so*|libm.so*|libpthread.so*|libdl.so*|librt.so*|libc.musl-*.so*) ;;
                    *) echo "Unexpected runtime dependency: $dep" >&2; exit 1 ;;
                esac
            done
        fi
        ;;
    *) echo 'Linkage verification unavailable on this host' >&2; exit 1 ;;
esac
