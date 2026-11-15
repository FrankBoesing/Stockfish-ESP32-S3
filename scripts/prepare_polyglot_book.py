#!/usr/bin/env python3
"""Validate and package a Polyglot .bin file for the Stockfish ESP32-S3 book partition."""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import zlib

ENTRY_SIZE = 16
MAGIC = b"SFBK"
VERSION = 1
HEADER = struct.Struct("<4sHHIIII")
PARTITION_SIZE = 0xBE0000


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate a sorted Polyglot .bin and prepend the ESP32 book-image header."
    )
    parser.add_argument("book", type=pathlib.Path, help="source Polyglot .bin file")
    parser.add_argument(
        "-o", "--output", type=pathlib.Path,
        help="output book image (default: source name with .sfbook suffix)",
    )
    args = parser.parse_args()

    source = args.book
    if not source.is_file():
        parser.error(f"input file does not exist: {source}")

    data = source.read_bytes()
    if not data or len(data) % ENTRY_SIZE:
        parser.error(
            f"invalid Polyglot file size {len(data)}; expected a non-zero multiple of {ENTRY_SIZE}"
        )

    if len(data) + HEADER.size > PARTITION_SIZE:
        parser.error(
            f"book image requires {len(data) + HEADER.size} bytes; partition has {PARTITION_SIZE}"
        )

    entry_count = len(data) // ENTRY_SIZE
    previous_key = -1
    for index in range(entry_count):
        offset = index * ENTRY_SIZE
        key = int.from_bytes(data[offset : offset + 8], "big")
        if key < previous_key:
            parser.error(f"Polyglot keys are not sorted at entry {index}")
        previous_key = key

    checksum = zlib.crc32(data) & 0xFFFFFFFF
    header = HEADER.pack(MAGIC, VERSION, HEADER.size, entry_count, len(data), checksum, 0)

    output = args.output or source.with_suffix(".sfbook")
    if output.resolve() == source.resolve():
        parser.error("output path must not overwrite the source .bin file")

    output.write_bytes(header + data)

    print(f"Input:       {source}")
    print(f"Entries:     {entry_count:,}")
    print(f"Data bytes:  {len(data):,}")
    print(f"CRC32:       {checksum:08X}")
    print(f"Image:       {output} ({len(header) + len(data):,} bytes)")
    print()
    print("Flash this image at 0x410000; do not flash the raw .bin directly:")
    print(
        'pio pkg exec -p tool-esptoolpy -- esptool --chip esp32s3 '
        '--port COM12 --baud 460800 write-flash 0x410000 '
        + f'"{output}"'
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
