# Flashing an opening book

This guide describes how to program a raw Polyglot `.bin` opening book into the
ESP32-S3's dedicated flash partition. The book is stored separately from the
firmware; no filesystem is used.

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
The application will access it read-only; an external programmer such as
esptool can still program it.

**The book offset is `0x410000`.** Do not use the application offset
`0x10000` or the partition-table offset `0x8000` for the book image.

## Initial book

The currently selected initial book is:

- File: `lichess_2100_rapid_2026-05.bin`
- Format: Polyglot binary
- Expected size: 1,601,616 bytes
- Source: [Lichess 2100 Rapid, May 2026 release](https://github.com/Dash1971/chess-opening-book-builder/releases/tag/books-2026-05-v1)
- License: CC0, according to the release metadata

Keep the raw `.bin` file local; do not commit it to the firmware repository.
The project `.gitignore` should contain the filename. Confirm with:

```powershell
git check-ignore -v .\\lichess_2100_rapid_2026-05.bin
```

## 1. Install the partition table and firmware

Before the first book flash, the ESP32 must have the matching single-app
partition table installed. From the PlatformIO project root, upload the GUI
firmware:

```powershell
pio run -e freenove_stockfish_gui -t upload
```

This uses `partitions_stockfish_16MB.csv` and the 4 MiB `factory` app
partition. The NVS partition remains at its previous offset and size
(`0x9000` / `0x5000`), so a normal firmware upload does not require a
full-chip erase.

**Do not run `erase-flash`, `erase-all`, or a PlatformIO full-chip erase.**
A full erase would also remove saved NVS settings and any previously flashed
book data.

The firmware/partition-table upload is required when moving from the old
partition layout. It is not necessary every time only the book contents are
being replaced.

## 2. Check the file

Run the following from the project root. The expected file size for the
selected book is exactly 1,601,616 bytes:

```powershell
Get-Item .\\lichess_2100_rapid_2026-05.bin | Select-Object Name, Length
```

If the file is in another directory, adjust the path in the command below.
Pass the raw `.bin` file, not the downloaded ZIP archive.

## 3. Flash the book

Close the PlatformIO serial monitor first. Replace `COM12` with the COM port
assigned to the ESP32-S3. Run:

```powershell
pio pkg exec -p tool-esptoolpy -- esptool --chip esp32s3 --port COM12 --baud 460800 write-flash 0x410000 ".\\lichess_2100_rapid_2026-05.bin"
```

The command uses PlatformIO's installed `tool-esptoolpy` package. The
successful board run for this project used esptool **v5.4.0** and a baud rate
of **460800**.

The `write-flash` command automatically erases the flash sectors needed for
the supplied image, then writes and verifies the data. It does not erase the
entire chip. A successful run ends with output similar to:

```text
esptool v5.4.0
Connected to ESP32-S3 on COM12:
...
Flash will be erased from 0x00410000 to 0x00597fff...
Compressed 1601616 bytes to 533481...
Wrote 1601616 bytes (533481 compressed) at 0x00410000 ...
Hash of data verified.
Hard resetting via RTS pin...
```

The key success indicator is:

```text
Hash of data verified.
```

This confirms that the bytes read back from flash match the input image.

## Replacing the book later

To replace the book, repeat only steps 2 and 3 with the new raw Polyglot
binary, keeping the destination offset at `0x410000`. The new image must fit
inside the 11.875 MiB book partition.

The reader must use the actual book-file length (or validated metadata) as its
end boundary. When a smaller file replaces a larger one, `write-flash` only
erases the sectors covered by the new file; old bytes can remain after its
end. Do not treat the complete partition size as the current book length.

A normal firmware upload with the same partition layout writes the application
and partition table, not the book contents. Avoid full-chip erase operations
when preserving the book is required.

## Current implementation status

The initial `lichess_2100_rapid_2026-05.bin` image has been programmed at
`0x410000`; esptool reported `Hash of data verified.` This validates the raw
flash write. It does **not** yet mean the chess application reads or uses the
book: Polyglot hashing, move decoding, partition discovery, mmap lookup and
legal-move filtering still need to be implemented and tested.
