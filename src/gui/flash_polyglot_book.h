#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <esp_partition.h>

#include "polyglot_book.h"

namespace PolyglotBook {

constexpr std::size_t ImageHeaderSize = 24;
constexpr uint16_t ImageVersion = 1;

inline uint16_t readLittleEndian16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

inline uint32_t readLittleEndian32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint32_t crc32(const uint8_t* data, std::size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

class FlashPolyglotBook {
public:
    FlashPolyglotBook() = default;
    FlashPolyglotBook(const FlashPolyglotBook&) = delete;
    FlashPolyglotBook& operator=(const FlashPolyglotBook&) = delete;

    ~FlashPolyglotBook() { close(); }

    bool begin() {
        if (ready_)
            return true;

        partition_ = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            static_cast<esp_partition_subtype_t>(0x06),
            "book");
        if (!partition_) {
            error_ = "book partition not found";
            return false;
        }

        if (partition_->size < ImageHeaderSize) {
            error_ = "book partition too small for image header";
            return false;
        }

        uint8_t header[ImageHeaderSize] = {};
        if (esp_partition_read(partition_, 0, header, sizeof(header)) != ESP_OK) {
            error_ = "cannot read book image header";
            return false;
        }

        if (std::memcmp(header, "SFBK", 4) != 0) {
            error_ = "book image header missing (prepare the .bin image first)";
            return false;
        }

        const uint16_t version = readLittleEndian16(header + 4);
        const uint16_t headerSize = readLittleEndian16(header + 6);
        const uint32_t entryCount = readLittleEndian32(header + 8);
        const uint32_t dataBytes = readLittleEndian32(header + 12);
        const uint32_t expectedCrc = readLittleEndian32(header + 16);
        const uint32_t flags = readLittleEndian32(header + 20);

        if (version != ImageVersion || headerSize != ImageHeaderSize || flags != 0) {
            error_ = "unsupported book image header version/flags";
            return false;
        }
        if (!entryCount || dataBytes != uint64_t(entryCount) * EntrySize
            || dataBytes > partition_->size - ImageHeaderSize) {
            error_ = "invalid book image length";
            return false;
        }

        const void* mapped = nullptr;
        const esp_err_t mapError = esp_partition_mmap(
            partition_, ImageHeaderSize, dataBytes, ESP_PARTITION_MMAP_DATA,
            &mapped, &mapHandle_);
        if (mapError != ESP_OK || !mapped) {
            error_ = "book data mmap failed (data address space may be full)";
            return false;
        }

        entries_ = static_cast<const uint8_t*>(mapped);
        entryCount_ = entryCount;
        dataBytes_ = dataBytes;

        if (crc32(entries_, dataBytes_) != expectedCrc) {
            close();
            error_ = "book CRC32 mismatch";
            return false;
        }

        uint64_t previousKey = 0;
        for (std::size_t i = 0; i < entryCount_; ++i) {
            const uint64_t key = readBigEndian64(entries_ + i * EntrySize);
            if (i && key < previousKey) {
                close();
                error_ = "Polyglot entries are not sorted by key";
                return false;
            }
            previousKey = key;
        }

        ready_ = true;
        error_ = nullptr;
        return true;
    }

    void close() {
        if (entries_) {
            esp_partition_munmap(mapHandle_);
            entries_ = nullptr;
            entryCount_ = 0;
            dataBytes_ = 0;
            mapHandle_ = 0;
        }
        ready_ = false;
    }

    bool ready() const { return ready_; }
    const char* error() const { return error_ ? error_ : "none"; }
    std::size_t entryCount() const { return entryCount_; }
    std::size_t dataBytes() const { return dataBytes_; }

    bool chooseMove(const Stockfish::Position& position,
                    uint64_t randomValue,
                    Stockfish::Move& selected,
                    SelectionStats* stats = nullptr) const {
        if (!ready_)
            return false;
        return PolyglotBook::chooseMove(
            position, entries_, entryCount_, randomValue, selected, stats);
    }

private:
    const esp_partition_t* partition_ = nullptr;
    const uint8_t* entries_ = nullptr;
    std::size_t entryCount_ = 0;
    std::size_t dataBytes_ = 0;
    esp_partition_mmap_handle_t mapHandle_ = 0;
    const char* error_ = nullptr;
    bool ready_ = false;
};

}  // namespace PolyglotBook
