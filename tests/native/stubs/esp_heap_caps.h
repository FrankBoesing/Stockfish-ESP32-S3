#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

// Minimal host-side substitute for the ESP-IDF heap-caps API. It deliberately
// supports forcing PSRAM allocation failure so GameHistory's internal-RAM
// fallback can be tested on a desktop runner.
constexpr uint32_t MALLOC_CAP_SPIRAM = 1u << 0;
constexpr uint32_t MALLOC_CAP_8BIT = 1u << 1;
constexpr uint32_t MALLOC_CAP_INTERNAL = 1u << 2;

inline bool test_fail_psram_allocations = false;

inline void* heap_caps_malloc(std::size_t size, uint32_t caps) {
  if ((caps & MALLOC_CAP_SPIRAM) && test_fail_psram_allocations)
    return nullptr;
  return std::malloc(size);
}

inline void heap_caps_free(void* ptr) {
  std::free(ptr);
}
