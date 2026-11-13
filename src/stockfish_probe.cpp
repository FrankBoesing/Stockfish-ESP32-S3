#include <Arduino.h>
#include <esp_heap_caps.h>

#include "stockfish/attacks.h"
#include "stockfish/position.h"
#include "stockfish/misc.h"
#include "stockfish/nnue/network.h"
#include "stockfish/nnue/nnue_accumulator.h"
#include "stockfish/search.h"

void setup() {
  Serial.begin(115200);
  delay(300);

  Stockfish::Attacks::init();
  Stockfish::Position::init();

  Serial.println();
  Serial.println("Stockfish 19 Smallnet compile/link probe");
  const std::string version = Stockfish::engine_version_info();
  Serial.println(version.c_str());

  using Stockfish::Eval::NNUE::Network;
  using Stockfish::Eval::NNUE::AccumulatorStack;
  using Stockfish::Eval::NNUE::AccumulatorCaches;
  using Stockfish::Search::Worker;

  Serial.printf("sizeof(Network)           = %u bytes\n", (unsigned)sizeof(Network));
  Serial.printf("sizeof(AccumulatorStack)  = %u bytes\n", (unsigned)sizeof(AccumulatorStack));
  Serial.printf("sizeof(AccumulatorCaches) = %u bytes\n", (unsigned)sizeof(AccumulatorCaches));
  Serial.printf("sizeof(Search::Worker)    = %u bytes\n", (unsigned)sizeof(Worker));
  Serial.printf("PSRAM free                = %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  Serial.printf("Internal free             = %u bytes\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  Serial.println("Probe complete. No engine, NNUE weights, or search worker was allocated.");
}

void loop() {
  delay(1000);
}
