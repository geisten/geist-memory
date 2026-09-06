#!/bin/sh
# Reproducible with the same inputs, toolchain, tar and gzip implementations.
set -eu
root=$1
archive=$2
export LC_ALL=C TZ=UTC
umask 022
epoch=${SOURCE_DATE_EPOCH:-946684800}
case "$epoch" in ''|*[!0-9]*) echo 'Invalid SOURCE_DATE_EPOCH' >&2; exit 1 ;; esac
if stamp=$(date -u -r "$epoch" +%Y%m%d%H%M.%S 2>/dev/null); then :
else stamp=$(date -u -d "@$epoch" +%Y%m%d%H%M.%S); fi
if command -v sha256sum >/dev/null 2>&1; then digest=sha256sum
else digest='shasum -a 256'; fi
$digest Makefile mk/config.mk patches/*.patch .clang-format include/*.h src/*.[ch] \
    test/*.[ch] test/fixtures/*.h test/*.sh examples/*.c tools/*.sh tools/*.py > "$root/SOURCE-SHA256SUMS"
scratch=$(mktemp -d "${archive}.tmp.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
cd "$root"
printf '%s\n' "SOURCE_DATE_EPOCH=$epoch" > PACKAGE.txt
# Hash every payload file, including licenses, documentation and pkg-config.
find usr -type f -print > "$scratch/files"
printf '%s\n' BUILD.txt PACKAGE.txt SOURCE-SHA256SUMS >> "$scratch/files"
sort "$scratch/files" -o "$scratch/files"
: > SHA256SUMS
while IFS= read -r file; do $digest "$file" >> SHA256SUMS; done < "$scratch/files"
find usr -type d -exec chmod 755 {} +
find usr -type f -exec chmod 644 {} +
chmod 644 BUILD.txt PACKAGE.txt SOURCE-SHA256SUMS SHA256SUMS
find usr -exec touch -t "$stamp" {} +
touch -t "$stamp" BUILD.txt PACKAGE.txt SOURCE-SHA256SUMS SHA256SUMS
find usr -print > "$scratch/entries"
printf '%s\n' BUILD.txt PACKAGE.txt SOURCE-SHA256SUMS SHA256SUMS >> "$scratch/entries"
sort "$scratch/entries" -o "$scratch/entries"
if tar --version | head -1 | grep -q 'GNU tar'; then
    tar --format=ustar --numeric-owner --owner=0 --group=0 --no-recursion \
        -cf "$scratch/out.tar" -T "$scratch/entries"
else
    tar --format=ustar --uid 0 --gid 0 --uname '' --gname '' --no-recursion \
        -cf "$scratch/out.tar" -T "$scratch/entries"
fi
# Separate commands preserve tar's failure status on POSIX shells without pipefail.
gzip -n -c "$scratch/out.tar" > "$scratch/out.tar.gz"
mv "$scratch/out.tar.gz" "$archive"
printf 'Package: %s\n' "$archive"
