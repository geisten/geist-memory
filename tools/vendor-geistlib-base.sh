#!/bin/sh
# Re-vendor geistlib's src/base headers into src/base/.
#
# geistlib installs only include/geist*.h. heap.h and checked.h are engine
# internals, so a consumer that wants the same allocation and overflow
# discipline has to carry a copy; this script makes that copy reproducible
# rather than a one-time hand edit.
#
#   sh tools/vendor-geistlib-base.sh [path-to-geistlib]
set -eu

SRC="${1:-../geistlib}"
[ -d "$SRC/src/base" ] || { echo "no $SRC/src/base — pass a geistlib checkout" >&2; exit 2; }
SHA=$(git -C "$SRC" rev-parse --short HEAD)

for f in checked.h heap.h; do
    out="src/base/$f"
    {
        printf '/*\n * Vendored from geistlib: src/base/%s @ %s\n *\n' "$f" "$SHA"
        printf ' * geistlib ships only include/geist*.h; heap.h and checked.h live under\n'
        printf ' * src/base and are not installable, so a consumer that wants the same\n'
        printf ' * allocation and overflow discipline has to carry a copy. This is that\n'
        printf ' * copy -- unmodified below this banner, so a diff against upstream is a\n'
        printf ' * plain file comparison. Re-vendor with tools/vendor-geistlib-base.sh.\n'
        if [ "$f" = "heap.h" ]; then
            printf ' *\n * HEADER ONLY. heap_alloc_aligned and friends are defined in geistlib'"'"'s\n'
            printf ' * heap.c and exported from libgeist.a, which this library already links, so\n'
            printf ' * nothing here needs a second implementation -- but it does mean geistlib'"'"'s\n'
            printf ' * archive is a link-time dependency of anything that calls them, not just a\n'
            printf ' * compile-time one.\n'
        fi
        printf ' */\n'
        cat "$SRC/src/base/$f"
    } > "$out"
    echo "vendored $out from $SHA"
done
