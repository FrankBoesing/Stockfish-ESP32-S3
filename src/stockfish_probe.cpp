#include <Arduino.h>
#include <Wire.h>
#include <LovyanGFX.hpp>
#include "board_config.h"

// Engine construction currently runs inside Arduino's loopTask (setup()).
// The core default is 8 KiB; increase it temporarily for stack diagnostics.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// Arduino defines HIGH/LOW as GPIO level macros. Stockfish uses the same
// identifiers in PrefetchLoc, so remove the macros after the board headers.
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
#include "stockfish/esp_trace.h"
#include "stockfish/nnue/network.h"
#include "stockfish/nnue/nnue_accumulator.h"
#if defined(STOCKFISH_NNUE_BENCHMARK) || defined(STOCKFISH_ESP32_S3_PIE_VALIDATE)
#include "stockfish/nnue/nnue_bench.h"
#endif
#if defined(STOCKFISH_NNUE_BENCHMARK)
namespace Stockfish::Eval::NNUE::Bench {
Counter counters[StageCount]{};
}
#endif
#if defined(STOCKFISH_ESP32_S3_PIE_VALIDATE)
namespace Stockfish::Eval::NNUE::Bench {
PieValidationStats pieValidationStats{};
}
#endif
#include "stockfish/history.h"
#include "stockfish/search.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

// Declared here because the search helpers are above the heap-report definition.
static void print_heap(const char* stage);

class BoardDisplay : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ILI9341 _panel;
public:
  BoardDisplay() {
    auto b = _bus.config();
    b.spi_host = SPI2_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.spi_3wire = false;
    b.use_lock = true;
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = LCD_SCLK;
    b.pin_mosi = LCD_MOSI;
    b.pin_miso = LCD_MISO;
    b.pin_dc = LCD_DC;
    _bus.config(b);
    _panel.setBus(&_bus);

    auto p = _panel.config();
    p.pin_cs = LCD_CS;
    p.pin_rst = LCD_RST;
    p.pin_busy = -1;
    p.memory_width = LCD_WIDTH;
    p.memory_height = LCD_HEIGHT;
    p.panel_width = LCD_WIDTH;
    p.panel_height = LCD_HEIGHT;
    p.offset_x = 0;
    p.offset_y = 0;
    p.offset_rotation = 0;
    p.dummy_read_pixel = 8;
    p.dummy_read_bits = 1;
    p.readable = true;
    p.invert = false;
    p.rgb_order = false;
    p.dlen_16bit = false;
    p.bus_shared = true;
    _panel.config(p);
    setPanel(&_panel);
  }
};
static BoardDisplay tft;

static bool touchRead(uint16_t &x, uint16_t &y, uint8_t &points) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TOUCH_I2C_ADDR, (uint8_t)5) != 5) return false;
  uint8_t count = Wire.read() & 0x0F;
  uint8_t xh = Wire.read(), xl = Wire.read();
  uint8_t yh = Wire.read(), yl = Wire.read();
  points = count;
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  return count != 0;
}

struct SearchProbeStats {
  bool started;
  int depth;
  uint64_t nodes;
  uint32_t nps;
  int hashfull;
  uint32_t stackFreeBytes;
  int coreId;
  char bestmove[16];
  char ponder[16];
};

static SearchProbeStats searchProbeStats{};

#if defined(STOCKFISH_NNUE_BENCHMARK)
static void resetNnueBenchCounters() {
  for (unsigned i = 0; i < Stockfish::Eval::NNUE::Bench::StageCount; ++i) {
    Stockfish::Eval::NNUE::Bench::counters[i].cycles = 0;
    Stockfish::Eval::NNUE::Bench::counters[i].calls = 0;
  }
}

static void printNnueBenchCounters(const char *label) {
  using namespace Stockfish::Eval::NNUE::Bench;
  static const char *const stageNames[StageCount] = {
    "accumulator", "feature-transform", "fc0", "sqr-relu0", "clip-relu0",
    "fc1", "sqr-relu1", "clip-relu1", "fc2", "evaluation-total",
    "accum-find-last", "accum-forward", "accum-backward", "accum-forward-both",
    "accum-refresh-cache", "accum-build-indices", "accum-main-loop",
    "accum-psqt-loop", "accum-update-both", "accum-pie-loop"
  };
  const uint64_t totalCycles = counters[EvaluationTotal].cycles;
  Serial.printf("[NNUEBENCH] %s evals=%u total=%llu cycles avg=%llu cycles/eval\n",
                label,
                (unsigned)counters[EvaluationTotal].calls,
                (unsigned long long)totalCycles,
                (unsigned long long)(counters[EvaluationTotal].calls
                  ? totalCycles / counters[EvaluationTotal].calls : 0));
  for (unsigned i = 0; i < StageCount; ++i) {
    const Counter &counter = counters[i];
    const uint64_t average = counter.calls ? counter.cycles / counter.calls : 0;
    const uint64_t shareTenths = totalCycles
      ? (counter.cycles * 1000ULL) / totalCycles : 0;
    Serial.printf("[NNUEBENCH] %-17s calls=%u avg=%llu cycles share=%llu.%01llu%%\n",
                  stageNames[i], (unsigned)counter.calls,
                  (unsigned long long)average,
                  (unsigned long long)(shareTenths / 10),
                  (unsigned long long)(shareTenths % 10));
  }
}
#else
static void resetNnueBenchCounters() {}
static void printNnueBenchCounters(const char *) {}
#endif

#if defined(STOCKFISH_ESP32_S3_PIE_VALIDATE)
static void resetPieValidationStats() {
  Stockfish::Eval::NNUE::Bench::pieValidationStats = {};
}

static void printPieValidationStats(const char *label) {
  using Stockfish::Eval::NNUE::Bench::pieValidationStats;
  const auto &stats = pieValidationStats;
  const char *status = stats.calls == 0 ? "NOT_RUN"
    : (stats.mismatchedCalls == 0 && stats.outOfRangeCalls == 0 ? "PASS" : "FAIL");
  Serial.printf("[PIE_VALIDATE] %s status=%s updates=%llu mismatched_updates=%llu mismatched_elements=%llu out_of_range_updates=%llu out_of_range_elements=%llu",
                label, status,
                (unsigned long long)stats.calls,
                (unsigned long long)stats.mismatchedCalls,
                (unsigned long long)stats.mismatchedElements,
                (unsigned long long)stats.outOfRangeCalls,
                (unsigned long long)stats.outOfRangeElements);
  if (stats.hasMismatch)
    Serial.printf(" first_mismatch_index=%u expected=%ld actual=%ld",
                  (unsigned)stats.firstMismatchIndex,
                  (long)stats.firstExpected, (long)stats.firstActual);
  Serial.println();
  Serial.printf("[PIE_VALIDATE] feature-shapes R1/A1=%llu R1/A2=%llu R2/A1=%llu R2/A2=%llu\\n",
                (unsigned long long)stats.featureShapeCounts[0],
                (unsigned long long)stats.featureShapeCounts[1],
                (unsigned long long)stats.featureShapeCounts[2],
                (unsigned long long)stats.featureShapeCounts[3]);
}
#else
static void resetPieValidationStats() {}
static void printPieValidationStats(const char *) {}
#endif



static void copySearchText(char *destination, size_t capacity, std::string_view source) {
  if (capacity == 0) return;
  const size_t count = source.size() < capacity - 1 ? source.size() : capacity - 1;
  if (count)
    memcpy(destination, source.data(), count);
  destination[count] = '\0';
}

static void sampleSearchStack() {
  const uint32_t freeBytes = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
  searchProbeStats.coreId = xPortGetCoreID();
  if (searchProbeStats.stackFreeBytes == 0 ||
      freeBytes < searchProbeStats.stackFreeBytes)
    searchProbeStats.stackFreeBytes = freeBytes;
}

static void resetSearchProbeStats() {
  memset(&searchProbeStats, 0, sizeof(searchProbeStats));
  searchProbeStats.coreId = -1;
}

static void printSearchProbeStats(const char *label) {
  Serial.printf("[SEARCH] %s started=%s depth=%d nodes=%llu nps=%u hashfull=%d "
                "worker-core=%d worker-stack-min-free=%u bytes bestmove=%s",
                label,
                searchProbeStats.started ? "yes" : "no",
                searchProbeStats.depth,
                (unsigned long long)searchProbeStats.nodes,
                (unsigned)searchProbeStats.nps,
                searchProbeStats.hashfull,
                searchProbeStats.coreId,
                (unsigned)searchProbeStats.stackFreeBytes,
                searchProbeStats.bestmove[0] ? searchProbeStats.bestmove : "(none)");
  if (searchProbeStats.ponder[0])
    Serial.printf(" ponder=%s", searchProbeStats.ponder);
  Serial.println();
  if (searchProbeStats.stackFreeBytes != 0 &&
      searchProbeStats.stackFreeBytes < 16 * 1024)
    Serial.println("[SEARCH] WARNING: less than 16 KiB minimum worker stack reserve.");
}

static void drawProbeDashboard() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(8, 8);
  tft.println("Stockfish ESP32");
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.println("Engine + display memory test");
  tft.drawFastHLine(8, 48, tft.width() - 16, TFT_DARKGREY);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(8, 62);
  tft.println("ILI9341 / LovyanGFX");
  tft.setCursor(8, 80);
  tft.println("FT6336U touch / I2C");
  tft.setCursor(8, 98);
  tft.println("Stockfish 19 Smallnet");
  tft.setCursor(8, 116);
  tft.println("1 worker / 1 MiB hash");
  tft.drawFastHLine(8, 144, tft.width() - 16, TFT_DARKGREY);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(8, 158);
  tft.println("Search tests: depth <= 16");
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(8, 178);
  tft.println("Node-capped: start / Kiwipete");

  const uint32_t bars[] = {
    TFT_RED, TFT_GREEN, TFT_BLUE, TFT_CYAN, TFT_MAGENTA, TFT_YELLOW
  };
  const int y = tft.height() - 22;
  const int w = tft.width() / 6;
  for (int i = 0; i < 6; ++i)
    tft.fillRect(i * w, y, w, 22, bars[i]);
}

static void initDisplayAndTouch() {
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, 1); // Arduino HIGH; macro is undefined for Stockfish headers.

  tft.init();
  tft.setRotation(1);
  Serial.printf("[GUI] Display initialized: %d x %d\n", tft.width(), tft.height());

  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, 1); // Arduino HIGH; macro is undefined for Stockfish headers.
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
  delay(50);
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  const bool touchPresent = Wire.endTransmission() == 0;
  Serial.printf("[GUI] FT6336U I2C @0x%02X: %s\n",
                TOUCH_I2C_ADDR, touchPresent ? "ACK" : "NOT FOUND");

  drawProbeDashboard();
}

static void runBoundedSearch(Stockfish::Engine& engine,
                              const char* label,
                              int maxDepth,
                              uint64_t nodeLimit) {
  resetSearchProbeStats();
  resetNnueBenchCounters();
  resetPieValidationStats();
  Stockfish::Search::LimitsType limits;
  limits.depth = maxDepth;
  limits.nodes = nodeLimit;
  limits.startTime = Stockfish::now();

  Serial.printf("\nStarting %s (max depth %d, max nodes %llu)...\n",
                label, maxDepth, (unsigned long long)nodeLimit);
  Serial.flush();
  engine.go(limits);
  engine.wait_for_search_finished();

  Serial.printf("%s complete.\n", label);
  printSearchProbeStats(label);
  printNnueBenchCounters(label);
  printPieValidationStats(label);
  char heapLabel[64];
  snprintf(heapLabel, sizeof(heapLabel), "After %.48s", label);
  print_heap(heapLabel);
}

static void runTimedSearch(Stockfish::Engine& engine, uint32_t milliseconds) {
  resetSearchProbeStats();
  resetNnueBenchCounters();
  resetPieValidationStats();
  Stockfish::Search::LimitsType limits;
  limits.movetime = milliseconds;
  limits.startTime = Stockfish::now();

  Serial.printf("\nStarting Stockfish timed search (%u ms)...\n",
                (unsigned)milliseconds);
  Serial.flush();
  engine.go(limits);
  engine.wait_for_search_finished();

  Serial.println("Timed search complete.");
  printSearchProbeStats("timed search");
  printNnueBenchCounters("timed search");
  printPieValidationStats("timed search");
  print_heap("After 5-second search");
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
  SF_ESP_TASK_INFO("Arduino setup/loop", -1);

  Serial.println();
  Serial.println("Stockfish 19 Smallnet ESP32-S3 engine memory probe");
  Serial.println("Compiler optimization = -Os");
  Serial.printf("ESP reset reason = %d\n", (int) esp_reset_reason());
  Serial.printf("Arduino loop task stack = %u bytes; current free stack ~= %u bytes\n",
                (unsigned)getArduinoLoopTaskStackSize(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  Serial.printf("CPU cores reported by Stockfish = %u; setup running on core %d\n",
                (unsigned)Stockfish::SYSTEM_THREADS_NB, (int)xPortGetCoreID());
  print_panic_configuration();
  Serial.flush();

  // Initialize the same ILI9341/FT6336U diagnostic UI used by the board
  // firmware before constructing the engine, to measure the combined budget.
  initDisplayAndTouch();
  print_heap("After display and touch initialization");

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
  print_heap("After GUI and core initialization");

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

  // Engine::go() verifies the network itself as well. Disable the reporting
  // callback to avoid printing the same verification message twice.
  engine.set_on_verify_network({});

  // The standalone Engine API expects the application to provide all search
  // callbacks. The UCI frontend normally installs these; this probe does not.
  // Search callbacks execute on the worker pthread, whose stack resides in
  // PSRAM. Keep them allocation-free and do not call Serial/ROM-heavy drivers.
  engine.set_on_start([]() {
    searchProbeStats.started = true;
    sampleSearchStack();
  });
  engine.set_on_update_no_moves([](const Stockfish::Engine::InfoShort&) {
    // No-move diagnostic is not needed for the start-position probe.
  });
  engine.set_on_update_full([](const Stockfish::Engine::InfoFull& info) {
    searchProbeStats.depth = info.depth;
    searchProbeStats.nodes = info.nodes;
    searchProbeStats.nps = (uint32_t)info.nps;
    searchProbeStats.hashfull = info.hashfull;
    sampleSearchStack();
  });
  engine.set_on_iter([](const Stockfish::Engine::InfoIter&) {
    // Current-move UCI output is intentionally omitted in this probe.
  });
  engine.set_on_bestmove([](std::string_view bestmove, std::string_view ponder) {
    copySearchText(searchProbeStats.bestmove, sizeof(searchProbeStats.bestmove), bestmove);
    copySearchText(searchProbeStats.ponder, sizeof(searchProbeStats.ponder), ponder);
    sampleSearchStack();
  });

  // Bound depth-16 searches by node count so this remains practical at the
  // current ~1 kNPS performance level. Reached depth is reported by callback.
  constexpr uint64_t ProbeNodeLimit = 30000;
  runBoundedSearch(engine, "start position depth-16 / 30k nodes", 16, ProbeNodeLimit);

  // Kiwipete is a legal, high-branching middlegame test position.
  const std::string kiwipeteFen =
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
  const auto positionError = engine.set_position(kiwipeteFen, {});
  if (positionError.has_value()) {
    Serial.printf("[POSITION] Kiwipete FEN rejected: %s\n", positionError->what());
  } else {
    Serial.printf("[POSITION] Kiwipete loaded: %s\n", engine.fen().c_str());
    runBoundedSearch(engine, "Kiwipete middlegame / depth-16 / 30k nodes",
                     16, ProbeNodeLimit);
  }

  // Restore the standard start position for subsequent application tests.
  const std::string startFen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  const auto restoreError = engine.set_position(startFen, {});
  if (restoreError.has_value())
    Serial.printf("[POSITION] start position restore failed: %s\n", restoreError->what());
  else
    Serial.println("[POSITION] start position restored.");

  runTimedSearch(engine, 5000);

  Serial.println();
  Serial.println("Probe complete. Display, NNUE, bounded depth-16 searches and timed search finished.");
}

void loop() {
  static uint32_t lastTouch = 0;
  uint16_t x = 0, y = 0;
  uint8_t points = 0;

  if (touchRead(x, y, points) && millis() - lastTouch > 100) {
    lastTouch = millis();
    Serial.printf("[GUI] touch points=%u x=%u y=%u\n",
                  (unsigned)points, (unsigned)x, (unsigned)y);
    tft.fillRect(8, 204, tft.width() - 16, 16, TFT_BLACK);
    tft.setCursor(8, 204);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.printf("Touch: %u, %u", (unsigned)x, (unsigned)y);
  }
  delay(10);
}
