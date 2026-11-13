#include <Arduino.h>

// Arduino defines HIGH/LOW as GPIO level macros. Stockfish uses the same
// identifiers in PrefetchLoc, so remove the macros before including it.
#ifdef HIGH
  #undef HIGH
#endif
#ifdef LOW
  #undef LOW
#endif

#include <esp_heap_caps.h>

#include "stockfish/engine.h"
#include "stockfish/attacks.h"
#include "stockfish/position.h"
#include "stockfish/misc.h"
#include "stockfish/nnue/network.h"
#include "stockfish/nnue/nnue_accumulator.h"
#include "stockfish/search.h"

static void print_heap(const char* stage) {
  constexpr uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  constexpr uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

  Serial.printf("\n[%s]\n", stage);
  Serial.printf("PSRAM free             = %u bytes\n",
                (unsigned)heap_caps_get_free_size(psramCaps));
  Serial.printf("PSRAM largest block    = %u bytes\n",
                (unsigned)heap_caps_get_largest_free_block(psramCaps));
  Serial.printf("PSRAM minimum free     = %u bytes\n",
                (unsigned)heap_caps_get_minimum_free_size(psramCaps));
  Serial.printf("Internal free          = %u bytes\n",
                (unsigned)heap_caps_get_free_size(internalCaps));
  Serial.printf("Internal largest block = %u bytes\n",
                (unsigned)heap_caps_get_largest_free_block(internalCaps));
  Serial.printf("Internal minimum free  = %u bytes\n",
                (unsigned)heap_caps_get_minimum_free_size(internalCaps));
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("Stockfish 19 Smallnet ESP32-S3 engine memory probe");

  Stockfish::Attacks::init();
  Stockfish::Position::init();

  using Stockfish::Eval::NNUE::Network;
  using Stockfish::Eval::NNUE::AccumulatorStack;
  using Stockfish::Eval::NNUE::AccumulatorCaches;
  using Stockfish::Search::Worker;

  Serial.println("Attack and position initialization complete.");
  Serial.printf("sizeof(Network)           = %u bytes (align %u)\n",
                (unsigned)sizeof(Network), (unsigned)alignof(Network));
  Serial.printf("sizeof(AccumulatorStack)  = %u bytes\n",
                (unsigned)sizeof(AccumulatorStack));
  Serial.printf("sizeof(AccumulatorCaches) = %u bytes\n",
                (unsigned)sizeof(AccumulatorCaches));
  Serial.printf("sizeof(Search::Worker)    = %u bytes (align %u)\n",
                (unsigned)sizeof(Worker), (unsigned)alignof(Worker));
  print_heap("After core initialization");

  Serial.println();
  Serial.println("Constructing Engine (1 worker, 1 MiB hash; no search)...");
  static Stockfish::Engine engine;

  Serial.println("Engine construction complete.");
  const std::string threadInfo = engine.thread_allocation_information_as_string();
  Serial.println(threadInfo.c_str());

  print_heap("After Engine construction");
  Serial.println();
  Serial.println("Probe complete. No search was started and NNUE verification was not requested.");
}

void loop() {
  delay(1000);
}
