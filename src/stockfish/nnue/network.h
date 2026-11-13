/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef NETWORK_H_INCLUDED
#define NETWORK_H_INCLUDED

#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <filesystem>

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
    #include <cstddef>
    #include <cstdlib>
    #include <new>
    #include <esp_heap_caps.h>
#endif

#include "../types.h"
#include "../misc.h"
#include "nnue_architecture.h"
#include "nnue_feature_transformer.h"
#include "nnue_misc.h"

namespace Stockfish {
class Position;
}

namespace Stockfish::Eval::NNUE {

class AccumulatorStack;
struct AccumulatorCaches;

using NetworkOutput = std::tuple<Value, Value>;

// The network must be a trivial type, i.e. the memory must be in-line.
// This is required to allow sharing the network via shared memory, as
// there is no way to run destructors.
class Network {
   public:
#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
    // Engine::get_default_network() initially uses std::unique_ptr<Network>.
    // Keep that ownership model, but place the large inline NNUE object in PSRAM.
    static void* operator new(std::size_t size) {
        return allocate_psram(size, alignof(Network));
    }

    static void* operator new(std::size_t size, std::align_val_t alignment) {
        return allocate_psram(size, static_cast<std::size_t>(alignment));
    }

    static void operator delete(void* ptr) noexcept {
        heap_caps_free(ptr);
    }

    static void operator delete(void* ptr, std::size_t) noexcept {
        heap_caps_free(ptr);
    }

    static void operator delete(void* ptr, std::align_val_t) noexcept {
        heap_caps_free(ptr);
    }

    static void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
        heap_caps_free(ptr);
    }

    // memory_allocator<T>() uses placement new; class allocation overloads
    // hide the global placement form unless it is explicitly provided.
    static void* operator new(std::size_t, void* ptr) noexcept { return ptr; }
    static void operator delete(void*, void*) noexcept {}

   private:
    static void* allocate_psram(std::size_t size, std::size_t alignment) {
        constexpr std::size_t MinimumAlignment = alignof(std::max_align_t);
        if (alignment < MinimumAlignment)
            alignment = MinimumAlignment;

        const std::size_t alignedSize = (size + alignment - 1) & ~(alignment - 1);
        void* ptr = heap_caps_aligned_alloc(
          alignment, alignedSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ptr)
            std::abort();
        return ptr;
    }

   public:
#endif
    Network() = default;

    Network(const Network& other) = default;
    Network(Network&& other)      = default;

    Network& operator=(const Network& other) = default;
    Network& operator=(Network&& other)      = default;

    void load(const std::filesystem::path& rootDirectory,
              std::filesystem::path        evalfilePath,
              EvalFile&                    evalFile);
    bool save(const EvalFile& evalFile, const std::optional<std::filesystem::path>& filename) const;

    usize get_content_hash() const;

    NetworkOutput evaluate(const Position&    pos,
                           AccumulatorStack&  accumulatorStack,
                           AccumulatorCaches& cache) const;


    void verify(const std::function<void(std::string_view)>& f,
                const EvalFile&                              evalFile,
                std::filesystem::path                        evalfilePath) const;

    NnueEvalTrace trace_evaluate(const Position&    pos,
                                 AccumulatorStack&  accumulatorStack,
                                 AccumulatorCaches& cache) const;

    void load_external(const std::filesystem::path&, const std::filesystem::path&, EvalFile&);
    void load_internal(EvalFile&);

   private:
    void initialize();

    bool                       save(std::ostream&, const std::string&) const;
    std::optional<std::string> load(std::istream&);

    bool read_header(std::istream&, u32*, std::string*) const;
    bool write_header(std::ostream&, u32, const std::string&) const;

    bool read_parameters(std::istream&, std::string&);
    bool write_parameters(std::ostream&, const std::string&) const;

    // Input feature converter
    FeatureTransformer featureTransformer;

    // Evaluation function
    NetworkArchitecture network[LayerStacks];

    bool initialized = false;

    // Hash value of evaluation function structure
    static constexpr u32 hash =
      FeatureTransformer::get_hash_value() ^ NetworkArchitecture::get_hash_value();

    friend struct AccumulatorCaches;
};


}  // namespace Stockfish::Eval::NNUE

template<>
struct std::hash<Stockfish::Eval::NNUE::Network> {
    Stockfish::usize operator()(const Stockfish::Eval::NNUE::Network& network) const noexcept {
        return network.get_content_hash();
    }
};

#endif
