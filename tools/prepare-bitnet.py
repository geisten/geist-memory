#!/usr/bin/env python3
"""Prepare one SHA-pinned Microsoft GGUF for the pinned geistlib engine.

Add projection-norm/last-token metadata and expand 1-D F16 norms exactly to F32.
Token embeddings and I2_S bytes remain unchanged. This optional setup tool needs
Python 3.11+, not NumPy or an additional library runtime. See MODEL_BENCHMARK.md.
"""
import hashlib
import os
from pathlib import Path
import struct
import sys
import tempfile

SOURCE_SHA = "c89c64f05a2d3f83565250a6762640197fc624df866d4a1bd5853f811219af17"
OUTPUT_SHA = "4321e21b9da533f40386aa5ab968cced6196e21ecbf395ae04e5bce1ee88e767"


def encoded_string(value):
    return struct.pack("<Q", len(value)) + value


def read_index(source):
    # Parsing occurs only after verification of the exact supported input hash.
    def number(fmt):
        return struct.unpack("<" + fmt, source.read(struct.calcsize("<" + fmt)))[0]

    def string():
        return source.read(number("Q"))

    def skip(kind):
        if kind == 8:
            string()
        elif kind == 9:
            element, count = number("I"), number("Q")
            for _ in range(count):
                skip(element)
        else:
            sizes = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 10: 8, 11: 8, 12: 8}
            source.seek(sizes[kind], 1)

    if source.read(4) != b"GGUF" or number("I") != 3:
        raise ValueError("expected GGUF v3")
    tensors, metadata = number("Q"), number("Q")
    for _ in range(metadata):
        string()
        skip(number("I"))
    metadata_end = source.tell()
    records = []
    for _ in range(tensors):
        name = string()
        dims = [number("Q") for _ in range(number("I"))]
        kind, offset = number("I"), number("Q")
        records.append((name, dims, kind, offset))
    data_start = (source.tell() + 31) // 32 * 32
    source.seek(0)
    prefix = bytearray(source.read(metadata_end))
    struct.pack_into("<Q", prefix, 16, metadata + 2)
    return prefix, records, data_start


def prepare(source, output):
    if hashlib.file_digest(source, "sha256").hexdigest() != SOURCE_SHA:
        raise ValueError("source SHA-256 does not match the supported official model")
    source.seek(0)
    prefix, records, data_start = read_index(source)
    extra = (
        encoded_string(b"bitnet.embedding.projection_input_norms") + struct.pack("<I?", 7, True)
        + encoded_string(b"bitnet.embedding.pooling") + struct.pack("<I", 8)
        + encoded_string(b"last_token")
    )
    index, copies, offset = bytearray(), [], 0
    for i, (name, dims, kind, old_offset) in enumerate(records):
        end = records[i + 1][3] if i + 1 < len(records) else os.fstat(source.fileno()).st_size - data_start
        size = end - old_offset
        count = dims[0] if kind == 1 and len(dims) == 1 else 0
        index += encoded_string(name) + struct.pack("<I", len(dims))
        index += struct.pack("<" + "Q" * len(dims), *dims)
        index += struct.pack("<IQ", 0 if count else kind, offset)
        copies.append((old_offset, size, count))
        offset += ((count * 4 if count else size) + 31) // 32 * 32

    fd, name = tempfile.mkstemp(prefix=output.name + ".", suffix=".tmp", dir=output.parent)
    temporary = Path(name)
    try:
        with os.fdopen(fd, "wb") as target:
            target.write(prefix + extra + index)
            target.write(bytes((-target.tell()) % 32))
            for old_offset, size, count in copies:
                source.seek(data_start + old_offset)
                if count:
                    values = struct.unpack("<" + "e" * count, source.read(count * 2))
                    target.write(struct.pack("<" + "f" * count, *values))
                else:
                    while size:
                        block = source.read(min(size, 1048576))
                        if not block:
                            raise ValueError("source was truncated during preparation")
                        target.write(block)
                        size -= len(block)
                target.write(bytes((-target.tell()) % 32))
            target.flush()
            os.fsync(target.fileno())
        with temporary.open("rb") as result:
            digest = hashlib.file_digest(result, "sha256").hexdigest()
        if digest != OUTPUT_SHA:
            raise ValueError("unexpected prepared model SHA-256")
        os.replace(temporary, output)
        print(f"prepared={output} sha256={digest}")
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: prepare-bitnet.py OFFICIAL_GGUF OUTPUT_GGUF")
    source_path, output_path = map(Path, sys.argv[1:])
    if source_path.resolve() == output_path.resolve():
        raise SystemExit("source and destination must differ")
    try:
        with source_path.open("rb") as source:
            prepare(source, output_path)
    except (OSError, ValueError) as error:
        raise SystemExit(str(error)) from error
