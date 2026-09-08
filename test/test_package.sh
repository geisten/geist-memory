#!/bin/sh
set -eu
work=$(mktemp -d "${TMPDIR:-/tmp}/geist-package-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
for copy in one two; do
    root="$work/$copy"
    mkdir -p "$root/usr/share/doc" "$root/usr/lib"
    printf 'documentation\n' > "$root/usr/share/doc/file with spaces.txt"
    printf 'archive fixture\n' > "$root/usr/lib/libfixture.a"
    printf 'fixed build configuration\n' > "$root/BUILD.txt"
done
chmod 700 "$work/two/usr"
chmod 400 "$work/two/usr/lib/libfixture.a"
touch -t 202001010000 "$work/two/usr/lib/libfixture.a"
SOURCE_DATE_EPOCH=946684800 sh tools/package.sh "$work/one" "$work/one.tar.gz" > /dev/null
SOURCE_DATE_EPOCH=946684800 sh tools/package.sh "$work/two" "$work/two.tar.gz" > /dev/null
cmp "$work/one.tar.gz" "$work/two.tar.gz"
mkdir "$work/unpack"
tar -xzf "$work/one.tar.gz" -C "$work/unpack"
(cd "$work/unpack" && if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -c SHA256SUMS; else shasum -a 256 -c SHA256SUMS; fi) > /dev/null
printf 'tampered\n' >> "$work/unpack/usr/share/doc/file with spaces.txt"
if (cd "$work/unpack" && if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -c SHA256SUMS; else shasum -a 256 -c SHA256SUMS; fi) > /dev/null 2>&1; then
    echo 'Tampered documentation escaped checksum validation' >&2; exit 1
fi
if SOURCE_DATE_EPOCH=invalid sh tools/package.sh "$work/one" "$work/one.tar.gz" > /dev/null 2>&1; then
    echo 'Invalid epoch accepted' >&2; exit 1
fi
cmp "$work/one.tar.gz" "$work/two.tar.gz"
mkdir "$work/bin"
for tool in tar gzip; do
    printf '#!/bin/sh\nexit 7\n' > "$work/bin/$tool"
    chmod 755 "$work/bin/$tool"
    if PATH="$work/bin:$PATH" sh tools/package.sh "$work/one" "$work/one.tar.gz" > /dev/null 2>&1; then
        echo "$tool failure was ignored" >&2; exit 1
    fi
    cmp "$work/one.tar.gz" "$work/two.tar.gz"
    rm "$work/bin/$tool"
done
SOURCE_DATE_EPOCH=946684802 sh tools/package.sh "$work/one" "$work/epoch.tar.gz" > /dev/null
if cmp -s "$work/one.tar.gz" "$work/epoch.tar.gz"; then
    echo 'Changed epoch did not affect package' >&2; exit 1
fi
printf 'PASS: package normalization, payload checksums and atomic publication on tool failures\n'
