# Flashing an opening book

This guide describes how to package a Polyglot `.bin` opening book and program
it into the ESP32-S3's dedicated flash partition. The book is stored separately
from the firmware; no filesystem is used.

## Flash layout

The target is the Freenove ESP32-S3 Display board with 16 MiB flash. The
partition table is `partitions_stockfish_16MB.csv`:

| Partition | Offset | Size |
|---|---:|---:|
| Factory application | `0x10000` | 4 MiB |
| Raw opening book | `0x410000` | 11.875 MiB |
| Coredump | `0xFF0000` | 64 KiB |

The book partition is named `book`, uses data subtype `0x06` (undefined), and
has the `readonly` flag. It is a raw data area, not SPIFFS, LittleFS or FAT.
The application accesses it read-only; an external programmer such as esptool
can still program it.

**The book image starts at `0x410000`.** Do not use the application offset
`0x10000` or the partition-table offset `0x8000` for the book image.

## Initial book

The selected initial book is:

- Source file: `lichess_2100_rapid_2026-05.bin`
- Format: Polyglot binary, 16-byte big-endian entries
- Raw data size: 1,601,616 bytes (100,101 entries)
- Source: [Lichess 2100 Rapid, May 2026 release](https://github.com/Dash1971/chess-opening-book-builder/releases/tag/books-2026-05-v1)
- License: CC0, according to the release metadata

Keep both the source `.bin` and generated `.sfbook` local; do not commit
book data to the firmware repository. The project `.gitignore` excludes these
files. Confirm with:

```powershell
git check-ignore -v .\lichess_2100_rapid_2026-05.bin
git check-ignore -v .\lichess_2100_rapid_2026-05.sfbook
```

## Image format

The ESP32 reader expects a 24-byte `SFBK` header followed by the unchanged
Polyglot entry data. The header stores the format version, header size, entry
count, data length and CRC32. This lets the firmware validate and map only the
actual book bytes; it does not have to guess where a book ends inside the
11.875 MiB partition.

The book payload remains ordinary Polyglot data:
- 8-byte Zobrist key, big-endian
- 2-byte encoded move, big-endian
- 2-byte weight, big-endian
- 4-byte learn field, big-endian

The packaging script verifies that the input size is a non-zero multiple of
16 and that entries are sorted by key. It then prepends the metadata header.
For the selected book, the resulting image is 1,601,640 bytes.

## 1. Update firmware and partition table

From the PlatformIO project root, update the local branch and upload the GUI
firmware containing the Polyglot reader:

```powershell
git pull
pio run -e freenove_stockfish_gui -t upload
```

The firmware uses `partitions_stockfish_16MB.csv` and the 4 MiB `factory`
app partition. NVS remains at its previous offset and size
(`0x9000` / `0x5000`), so a normal firmware upload does not require a
full-chip erase.

**Do not run `erase-flash`, `erase-all`, or a PlatformIO full-chip erase.**
A full erase would remove saved NVS settings and any previously flashed book.

The firmware/partition-table upload is required when moving to a new partition
layout or a new reader version. It is not necessary every time only the book
contents are being replaced.

## 2. Prepare the book image

Run the packaging script from the project root:

```powershell
python scripts/prepare_polyglot_book.py .\lichess_2100_rapid_2026-05.bin
```

The script creates:

```text
lichess_2100_rapid_2026-05.sfbook
```

It prints the entry count, raw data size, CRC32 and final image size. It exits
with an error if the input is malformed, unsorted or too large for the book
partition.

The raw `.bin` remains unchanged. Flash the generated `.sfbook`, not the
raw `.bin` file.

## 3. Flash the book

Close the PlatformIO serial monitor first. Replace `COM12` with the COM port
assigned to the ESP32-S3. Run:

```powershell
pio pkg exec -p tool-esptoolpy -- esptool --chip esp32s3 --port COM12 --baud 460800 write-flash 0x410000 ".\lichess_2100_rapid_2026-05.sfbook"
```

The command uses PlatformIO's installed `tool-esptoolpy` package. The board
has been tested with esptool **v5.4.0** and a baud rate of **460800**.

The `write-flash` command automatically erases the flash sectors needed for
the supplied image, then writes and verifies the data. It does not erase the
entire chip. A successful run ends with output similar to:

```text
esptool v5.4.0
Connected to ESP32-S3 on COM12:
...
Flash will be erased from 0x00410000 to ...
Wrote 1601640 bytes (...) at 0x00410000 ...
Hash of data verified.
Hard resetting via RTS pin...
```

The key success indicator is:

```text
Hash of data verified.
```

This confirms that the bytes read back from flash match the image.

## Replacing the book later

To replace the book, repeat steps 2 and 3 with another sorted Polyglot
`.bin` file. Keep the destination offset at `0x410000`. The resulting image,
including its 24-byte header, must fit inside the 11.875 MiB book partition.

The header records the exact payload length and entry count. The firmware
checks the CRC32 and sorted key order at startup, then maps only the actual
book payload. It does not map the full 11.875 MiB partition or copy the book
into RAM.

A normal firmware upload with the same partition layout writes the application
and partition table, not the book contents. Avoid full-chip erase operations
when preserving the book is required.

## Current implementation status

The initial raw `lichess_2100_rapid_2026-05.bin` was programmed before the
reader's image header was introduced. That raw image is **not** accepted by
the current reader. Repackage it with step 2 and flash the resulting
`.sfbook` image with step 3 before the on-device reader test.

Native tests now cover Polyglot hashing, move decoding (including castling,
en-passant and promotion), binary search and weighted legal-move selection.
The actual mmap, startup CRC validation and GUI fallback still require the
hardware test on the FNK0104B.
