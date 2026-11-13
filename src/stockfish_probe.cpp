#include <Arduino.h>

// Arduino defines HIGH/LOW as GPIO level macros. Stockfish uses the same
// identifiers in PrefetchLoc, so remove the macros before including it.
#ifdef HIGH
  #undef HIGH
#endif
#ifdef LOW
  #undef LOW
#endif
#ifdef cli
  #undef cli
#endif

#include <esp_heap_caps.h>
#include <esp_system.h>

#include "stockfish/engine.h"
#include "stockfish/attacks.h"
#include "stockfish/position.h"
#include "stockfish/misc.h"
#include "stockfish/nnue/network.h"
#include "stockfish/nnue/nnue_accumulator.h"
#include "stockfish/history.h"
#include "stockfish/search.h"

#include <cstdarg>
#include <cstdio>

void stockfish_esp_trace(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  Serial.printf("[SFTRACE] %s\n", buffer);
  Serial.flush();
}

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
  delay(2000);

  Serial.println();
  Serial.println("Stockfish 19 Smallnet ESP32-S3 engine memory probe");
  Serial.printf("ESP reset reason = %d\n", (int) esp_reset_reason());
  Serial.flush();

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
  Serial.printf("sizeof(ContinuationHistoryBlock) = %u bytes\n",
                (unsigned)sizeof(Stockfish::ContinuationHistoryBlock));
  Serial.printf("Pawn history entries      = %u\n",
                (unsigned)Stockfish::PAWN_HISTORY_BASE_SIZE);
  Serial.printf("Correction history entries= %u\n",
                (unsigned)Stockfish::CORRHIST_BASE_SIZE);
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
