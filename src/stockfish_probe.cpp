#include <Arduino.h>

// Engine construction currently runs inside Arduino's loopTask (setup()).
// The core default is 8 KiB; increase it temporarily for stack diagnostics.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

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
  const char* taskName = pcTaskGetName(nullptr);
  const UBaseType_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
  Serial.printf("[SFTRACE][%s stack-free~%u] %s\n",
                taskName ? taskName : "?",
                (unsigned)stackFree,
                buffer);
  Serial.flush();
}

static void print_panic_configuration() {
#if CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT
  Serial.println("Panic mode = SILENT_REBOOT");
#elif CONFIG_ESP_SYSTEM_PANIC_PRINT_HALT
  Serial.println("Panic mode = PRINT_HALT");
#elif CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT
  Serial.println("Panic mode = PRINT_REBOOT");
#elif CONFIG_ESP_SYSTEM_PANIC_GDBSTUB
  Serial.println("Panic mode = GDBSTUB");
#else
  Serial.println("Panic mode = unknown");
#endif

#if CONFIG_ESP_CONSOLE_USB_CDC
  Serial.println("IDF panic/stdio console = USB_CDC");
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
  Serial.println("IDF panic/stdio console = USB_SERIAL_JTAG");
#elif CONFIG_ESP_CONSOLE_UART
  Serial.println("IDF panic/stdio console = UART");
#elif CONFIG_ESP_CONSOLE_NONE
  Serial.println("IDF panic/stdio console = NONE");
#else
  Serial.println("IDF panic/stdio console = unknown");
#endif
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
  Serial.printf("Arduino loop task stack = %u bytes; current free stack ~= %u bytes\n",
                (unsigned)getArduinoLoopTaskStackSize(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  print_panic_configuration();
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

  // First verify that the expected NNUE network is loaded and compatible.
  // Stockfish::Engine::go() performs this check again before starting search.
  engine.set_on_verify_network([](std::string_view msg) {
    Serial.printf("[NNUE] %.*s\n", (int)msg.size(), msg.data());
  });

  Serial.println();
  Serial.println("Starting explicit NNUE network verification...");
  Serial.flush();
  engine.verify_network();
  Serial.println("NNUE verification complete.");
  print_heap("After NNUE verification");

  // A depth-1 search is the first test that exercises the real search path
  // and NNUE evaluation. Keep this deliberately short for the initial probe.
  Stockfish::Search::LimitsType limits;
  limits.depth = 1;

  Serial.println();
  Serial.println("Starting Stockfish depth-1 search...");
  Serial.flush();
  engine.go(limits);
  engine.wait_for_search_finished();

  Serial.println("Depth-1 search complete.");
  print_heap("After depth-1 search");

  Serial.println();
  Serial.println("Probe complete. NNUE verification and depth-1 search succeeded.");
}

void loop() {
  delay(1000);
}
