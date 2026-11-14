#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <esp_heap_caps.h>

// A move is stored in 16 bits: 6-bit source, 6-bit destination, 3-bit promotion.
// PSRAM is preferred; internal 8-bit-capable RAM is used as a fallback.
class GameHistory {
public:
  static constexpr size_t MaxMoves = 4096;
  GameHistory() = default;
  ~GameHistory() { if (moves_) heap_caps_free(moves_); }
  GameHistory(const GameHistory &) = delete;
  GameHistory &operator=(const GameHistory &) = delete;

  bool appendUci(std::string_view uci) {
    uint16_t packed = 0;
    if (!encodeUci(uci, packed) || size_ >= MaxMoves || !reserve(size_ + 1))
      return false;
    moves_[size_++] = packed;
    return true;
  }
  bool popBack() { if (!size_) return false; --size_; return true; }
  void clear() { size_ = 0; }
  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  void swap(GameHistory &other) {
    uint16_t *moves = moves_; moves_ = other.moves_; other.moves_ = moves;
    size_t size = size_; size_ = other.size_; other.size_ = size;
    size_t capacity = capacity_; capacity_ = other.capacity_; other.capacity_ = capacity;
    uint32_t caps = memoryCaps_; memoryCaps_ = other.memoryCaps_; other.memoryCaps_ = caps;
  }
  const uint16_t *packedData() const { return moves_; }
  bool usesPsram() const { return (memoryCaps_ & MALLOC_CAP_SPIRAM) != 0; }

  bool formatMoveAt(size_t index, char out[6]) const {
    return index < size_ && decodeUci(moves_[index], out);
  }
  std::vector<std::string> toUciMoves() const {
    std::vector<std::string> result;
    result.reserve(size_);
    char move[6];
    for (size_t i = 0; i < size_; ++i)
      if (decodeUci(moves_[i], move)) result.emplace_back(move);
    return result;
  }
  bool assignPacked(const uint16_t *packed, size_t count) {
    if (count > MaxMoves || (count && !packed)) return false;
    for (size_t i = 0; i < count; ++i) {
      char move[6];
      if (!decodeUci(packed[i], move)) return false;
    }
    if (!reserve(count)) return false;
    if (count) std::memcpy(moves_, packed, count * sizeof(uint16_t));
    size_ = count;
    return true;
  }
  bool assignPackedBytes(const uint8_t *packedBytes, size_t count) {
    if (count > MaxMoves || (count && !packedBytes)) return false;
    for (size_t i = 0; i < count; ++i) {
      uint16_t packed = 0;
      char move[6];
      std::memcpy(&packed, packedBytes + i * sizeof(uint16_t), sizeof(packed));
      if (!decodeUci(packed, move)) return false;
    }
    if (!reserve(count)) return false;
    if (count) std::memcpy(moves_, packedBytes, count * sizeof(uint16_t));
    size_ = count;
    return true;
  }

private:
  static bool encodeUci(std::string_view uci, uint16_t &packed) {
    if (uci.size() != 4 && uci.size() != 5) return false;
    if (uci[0] < 'a' || uci[0] > 'h' || uci[1] < '1' || uci[1] > '8'
        || uci[2] < 'a' || uci[2] > 'h' || uci[3] < '1' || uci[3] > '8')
      return false;
    const unsigned from = unsigned(uci[1] - '1') * 8u + unsigned(uci[0] - 'a');
    const unsigned to = unsigned(uci[3] - '1') * 8u + unsigned(uci[2] - 'a');
    if (from == to) return false;
    unsigned promotion = 0;
    if (uci.size() == 5) {
      switch (uci[4]) {
        case 'q': promotion = 1; break;
        case 'r': promotion = 2; break;
        case 'b': promotion = 3; break;
        case 'n': promotion = 4; break;
        default: return false;
      }
    }
    packed = uint16_t(from | (to << 6) | (promotion << 12));
    return true;
  }
  static bool decodeUci(uint16_t packed, char out[6]) {
    const unsigned from = packed & 0x3Fu;
    const unsigned to = (packed >> 6) & 0x3Fu;
    const unsigned promotion = (packed >> 12) & 0x7u;
    if ((packed & 0x8000u) || from == to || promotion > 4) return false;
    out[0] = char('a' + (from & 7u));
    out[1] = char('1' + (from >> 3));
    out[2] = char('a' + (to & 7u));
    out[3] = char('1' + (to >> 3));
    out[4] = '\0';
    if (promotion) {
      static constexpr char PromotionChars[] = "qrbn";
      out[4] = PromotionChars[promotion - 1];
      out[5] = '\0';
    }
    return true;
  }
  bool reserve(size_t required) {
    if (required <= capacity_) return true;
    if (required > MaxMoves) return false;
    size_t nextCapacity = capacity_ ? capacity_ : 32;
    while (nextCapacity < required)
      nextCapacity = nextCapacity > MaxMoves / 2 ? MaxMoves : nextCapacity * 2;
    const size_t bytes = nextCapacity * sizeof(uint16_t);
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint16_t *next = static_cast<uint16_t *>(heap_caps_malloc(bytes, caps));
    if (!next) {
      caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
      next = static_cast<uint16_t *>(heap_caps_malloc(bytes, caps));
    }
    if (!next) return false;
    if (size_) std::memcpy(next, moves_, size_ * sizeof(uint16_t));
    if (moves_) heap_caps_free(moves_);
    moves_ = next;
    capacity_ = nextCapacity;
    memoryCaps_ = caps;
    return true;
  }
  uint16_t *moves_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
  uint32_t memoryCaps_ = 0;
};
