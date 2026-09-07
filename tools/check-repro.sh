#!/bin/sh
# Called by Make: preserve compiler/profile overrides via inherited MAKEFLAGS.
set -eu
engine=$1
make_command=$2
work=$(mktemp -d "${TMPDIR:-/tmp}/geist-repro.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
for copy in one two; do
    mkdir "$work/$copy"
    cp -R Makefile mk patches src include test examples docs tools LICENSE README.md \
        PLAN.md CONTRIBUTING.md CHANGELOG.md .clang-format .gitattributes "$work/$copy/"
    if ! "$make_command" -C "$work/$copy" GEISTLIB="$engine" dist > "$work/$copy.log" 2>&1; then
        cat "$work/$copy.log" >&2
        exit 1
    fi
done
# Use the manifest to locate the resolved configuration without assuming a hash.
for copy in one two; do
    (cd "$work/$copy" && find build -name 'geist-memory-*.tar.gz' -print) > "$work/$copy.path"
    test "$(wc -l < "$work/$copy.path" | tr -d ' ')" = 1
    cp "$work/$copy/$(cat "$work/$copy.path")" "$work/$copy.tar.gz"
done
cmp "$work/one.tar.gz" "$work/two.tar.gz"
mkdir "$work/unpacked"
tar -xzf "$work/one.tar.gz" -C "$work/unpacked"
(cd "$work/unpacked" && if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -c SHA256SUMS; else shasum -a 256 -c SHA256SUMS; fi)
printf 'PASS: two fresh builds produced byte-identical packages with verified payloads\n'
