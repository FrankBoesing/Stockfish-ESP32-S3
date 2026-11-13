#include <Arduino.h>
#include <Wire.h>
#include <LovyanGFX.hpp>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>
#include "board_config.h"

class BoardDisplay : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ILI9341 _panel;
public:
  BoardDisplay() {
    auto b = _bus.config();
    b.spi_host = SPI2_HOST; b.spi_mode = 0;
    b.freq_write = 40000000; b.freq_read = 16000000;
    b.spi_3wire = false; b.use_lock = true; b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = LCD_SCLK; b.pin_mosi = LCD_MOSI; b.pin_miso = LCD_MISO; b.pin_dc = LCD_DC;
    _bus.config(b); _panel.setBus(&_bus);
    auto p = _panel.config();
    p.pin_cs = LCD_CS; p.pin_rst = LCD_RST; p.pin_busy = -1;
    p.memory_width = LCD_WIDTH; p.memory_height = LCD_HEIGHT;
    p.panel_width = LCD_WIDTH; p.panel_height = LCD_HEIGHT;
    p.offset_x = 0; p.offset_y = 0; p.offset_rotation = 0;
    p.dummy_read_pixel = 8; p.dummy_read_bits = 1;
    p.readable = true; p.invert = false; p.rgb_order = false;
    p.dlen_16bit = false; p.bus_shared = true;
    _panel.config(p); setPanel(&_panel);
  }

  // LovyanGFX Bus_SPI supports changing the write clock without rebuilding
  // the panel object. The new setting takes effect on the next SPI transfer.
  void setWriteClock(uint32_t hz) { _bus.setClock(hz); }
};
BoardDisplay tft;

static bool touchRead(uint16_t &x, uint16_t &y, uint8_t &points) {
  Wire.beginTransmission(TOUCH_I2C_ADDR); Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TOUCH_I2C_ADDR, (uint8_t)5) != 5) return false;
  uint8_t count = Wire.read() & 0x0F;
  uint8_t xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  points = count; x = ((xh & 0x0F) << 8) | xl; y = ((yh & 0x0F) << 8) | yl;
  return count != 0;
}
static void printMemory(const char *label, uint32_t caps) {
  Serial.printf("%-19s free=%7u largest=%7u total=%7u bytes\n", label,
    (unsigned)heap_caps_get_free_size(caps),
    (unsigned)heap_caps_get_largest_free_block(caps),
    (unsigned)heap_caps_get_total_size(caps));
}
static void memoryReport() {
  Serial.println("\n=== MEMORY ===");
  printMemory("8-bit accessible", MALLOC_CAP_8BIT);
  printMemory("Internal RAM", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  printMemory("PSRAM", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  Serial.printf("ESP free heap: %u\nESP heap size: %u\nPSRAM size: %u\nPSRAM free: %u\n",
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(),
    (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t testSize = largest > 65536 ? largest - 16384 : 0;
  if (testSize > 2 * 1024 * 1024) testSize = 2 * 1024 * 1024;
  uint8_t *mem = nullptr;
  // Probe close to the reported largest block; back off in 64 KiB steps if needed.
  while (testSize > 65536 && !(mem = (uint8_t *)heap_caps_malloc(testSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {
    testSize = testSize > 65536 ? testSize - 65536 : 0;
  }
  Serial.printf("PSRAM largest block: %u bytes; largest test allocation: %u bytes\n",
    (unsigned)largest, (unsigned)(mem ? testSize : 0));
  if (!mem) { Serial.println("PSRAM allocation: FAILED"); return; }
  uint64_t t0 = esp_timer_get_time();
  for (size_t i=0; i<testSize; ++i) mem[i] = (uint8_t)((i*37u) ^ (i>>7) ^ 0xA5u);
  uint32_t checksum = 0; bool ok = true;
  for (size_t i=0; i<testSize; ++i) {
    uint8_t expected = (uint8_t)((i*37u) ^ (i>>7) ^ 0xA5u);
    if (mem[i] != expected) { ok = false; break; }
    checksum += mem[i];
  }
  uint64_t elapsed = esp_timer_get_time() - t0;
  Serial.printf("PSRAM write/read: %s checksum=%08" PRIx32 " combined=%.2f MiB/s\n",
    ok ? "PASS" : "FAIL", checksum,
    elapsed ? (2.0 * testSize / (1024.0*1024.0)) / (elapsed/1000000.0) : 0.0);
  heap_caps_free(mem);
}


struct TTBenchEntry {
  uint32_t key;
  uint32_t value;
  uint32_t meta;
  uint32_t pad;
};
static_assert(sizeof(TTBenchEntry) == 16, "TT benchmark entries must be 16 bytes");
static volatile uint32_t memoryBenchSink = 0;

static inline uint32_t benchRandom(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

static void runMemoryCase(const char *label, uint32_t caps, size_t bytes,
                          uint32_t randomOps, uint32_t sequentialPasses) {
  uint8_t *raw = (uint8_t *)heap_caps_malloc(bytes, caps);
  if (!raw) {
    Serial.printf("MEMBENCH %s: allocation FAILED (%u bytes)\n",
                  label, (unsigned)bytes);
    return;
  }

  // Sequential 32-bit write/read bandwidth. Volatile accesses prevent the
  // compiler from eliminating the memory traffic.
  volatile uint32_t *words = (volatile uint32_t *)raw;
  const size_t wordCount = bytes / sizeof(uint32_t);
  uint64_t t0 = esp_timer_get_time();
  for (uint32_t pass = 0; pass < sequentialPasses; ++pass) {
    uint32_t seed = 0x9E3779B9u * (pass + 1);
    for (size_t i = 0; i < wordCount; ++i)
      words[i] = seed ^ (uint32_t)i;
  }
  uint64_t writeUs = esp_timer_get_time() - t0;

  uint32_t sum = 0;
  t0 = esp_timer_get_time();
  for (uint32_t pass = 0; pass < sequentialPasses; ++pass) {
    for (size_t i = 0; i < wordCount; ++i)
      sum += words[i];
  }
  uint64_t readUs = esp_timer_get_time() - t0;
  memoryBenchSink ^= sum;

  const double totalMiB = ((double)bytes * sequentialPasses) / 1048576.0;
  const double writeMiBs = writeUs ? totalMiB * 1000000.0 / writeUs : 0.0;
  const double readMiBs = readUs ? totalMiB * 1000000.0 / readUs : 0.0;

  // Randomized 16-byte entry accesses, modeling the size of a compact
  // transposition-table slot. This is a synthetic access test, not Stockfish.
  volatile TTBenchEntry *entries = (volatile TTBenchEntry *)raw;
  const uint32_t entryCount = (uint32_t)(bytes / sizeof(TTBenchEntry));
  const uint32_t indexMask = entryCount - 1;
  for (uint32_t i = 0; i < entryCount; ++i) {
    entries[i].key = 0xA5A50000u ^ i;
    entries[i].value = i * 17u;
    entries[i].meta = i ^ 0x5A5A5A5Au;
    entries[i].pad = 0;
  }

  uint32_t rng = 0xC001D00Du;
  uint32_t sink = 0;
  t0 = esp_timer_get_time();
  for (uint32_t i = 0; i < randomOps; ++i) {
    const uint32_t index = benchRandom(rng) & indexMask;
    const volatile TTBenchEntry &e = entries[index];
    const uint32_t key = e.key;
    const uint32_t value = e.value;
    const uint32_t meta = e.meta;
    sink ^= key + value + meta;
  }
  uint64_t probeUs = esp_timer_get_time() - t0;

  rng = 0x51A7E123u;
  t0 = esp_timer_get_time();
  for (uint32_t i = 0; i < randomOps; ++i) {
    const uint32_t index = benchRandom(rng) & indexMask;
    volatile TTBenchEntry &e = entries[index];
    e.key = rng;
    e.value = i ^ rng;
    e.meta = (rng >> 7) ^ i;
  }
  uint64_t storeUs = esp_timer_get_time() - t0;
  memoryBenchSink ^= sink ^ entries[benchRandom(rng) & indexMask].key;

  const double probeNs = randomOps ? (double)probeUs * 1000.0 / randomOps : 0.0;
  const double storeNs = randomOps ? (double)storeUs * 1000.0 / randomOps : 0.0;
  const double probeMops = probeUs ? (double)randomOps / probeUs : 0.0;
  const double storeMops = storeUs ? (double)randomOps / storeUs : 0.0;

  Serial.printf("MEMBENCH %-14s size=%u bytes entries=%u\n",
                label, (unsigned)bytes, (unsigned)entryCount);
  Serial.printf("  sequential: write=%7.2f MiB/s read=%7.2f MiB/s passes=%u\n",
                writeMiBs, readMiBs, (unsigned)sequentialPasses);
  Serial.printf("  random probe: %7.1f ns/op (%6.3f Mops/s)\n",
                probeNs, probeMops);
  Serial.printf("  random store: %7.1f ns/op (%6.3f Mops/s)\n",
                storeNs, storeMops);
  heap_caps_free(raw);
}


struct alignas(64) SharedMemoryWorker {
  TTBenchEntry *entries;
  uint32_t entryCount;
  uint32_t operations;
  uint32_t seed;
  uint32_t doStore;
  uint32_t done;
  uint32_t checksum;
  uint64_t elapsedUs;
  uint32_t core;
};
static uint32_t sharedMemoryBenchStart = 0;

static void sharedMemoryWorker(void *arg) {
  SharedMemoryWorker *w = (SharedMemoryWorker *)arg;
  while (!__atomic_load_n(&sharedMemoryBenchStart, __ATOMIC_ACQUIRE))
    taskYIELD();

  uint32_t rng = w->seed;
  uint32_t checksum = 0;
  const uint64_t startUs = esp_timer_get_time();

  for (uint32_t i = 0; i < w->operations; ++i) {
    const uint32_t r = benchRandom(rng);
    // Multiply-high maps the 32-bit PRNG output across arbitrary entry counts
    // (7 MiB is not a power-of-two number of 16-byte entries).
    const uint32_t index = (uint32_t)(((uint64_t)r * w->entryCount) >> 32);
    TTBenchEntry *e = &w->entries[index];

    if (w->doStore) {
      // Relaxed 32-bit atomics avoid C++ data races when both cores happen
      // to select the same slot. They do not provide a Stockfish TT protocol.
      __atomic_store_n(&e->key, r, __ATOMIC_RELAXED);
      __atomic_store_n(&e->value, i ^ r, __ATOMIC_RELAXED);
      __atomic_store_n(&e->meta, (r >> 7) ^ i, __ATOMIC_RELAXED);
    } else {
      checksum ^= __atomic_load_n(&e->key, __ATOMIC_RELAXED);
      checksum += __atomic_load_n(&e->value, __ATOMIC_RELAXED);
      checksum ^= __atomic_load_n(&e->meta, __ATOMIC_RELAXED);
    }
  }

  w->elapsedUs = esp_timer_get_time() - startUs;
  w->checksum = checksum ^ rng;
  __atomic_store_n(&w->done, 1, __ATOMIC_RELEASE);
  vTaskDelete(nullptr);
}

static void runSharedMemoryMode(TTBenchEntry *entries, uint32_t entryCount,
                                const char *sizeLabel, bool doStore,
                                uint32_t workerCount) {
  const uint32_t operations = 300000;
  SharedMemoryWorker workers[2] = {};
  TaskHandle_t handles[2] = {nullptr, nullptr};
  uint32_t created = 0;
  __atomic_store_n(&sharedMemoryBenchStart, 0, __ATOMIC_RELEASE);

  for (uint32_t i = 0; i < workerCount; ++i) {
    workers[i].entries = entries;
    workers[i].entryCount = entryCount;
    workers[i].operations = operations;
    workers[i].seed = 0xC001D00Du ^ (0x9E3779B9u * (i + 1));
    workers[i].doStore = doStore ? 1u : 0u;
    workers[i].done = 0;
    workers[i].checksum = 0;
    workers[i].elapsedUs = 0;
    // Single-core and dual-core runs both use core 1 as the first worker.
    // The second worker is added on core 0 for the dual-core run.
    workers[i].core = (workerCount == 1) ? 1u : (i == 0 ? 1u : 0u);
    char taskName[16];
    snprintf(taskName, sizeof(taskName), "mem_%u", (unsigned)i);
    BaseType_t ok = xTaskCreatePinnedToCore(
      sharedMemoryWorker, taskName, 4096, &workers[i], 3,
      &handles[i], (BaseType_t)workers[i].core);
    if (ok != pdPASS) break;
    ++created;
  }

  if (created != workerCount) {
    __atomic_store_n(&sharedMemoryBenchStart, 1, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < created; ++i)
      while (!__atomic_load_n(&workers[i].done, __ATOMIC_ACQUIRE)) taskYIELD();
    Serial.printf("MEMSHARED %s %s cores=%u: task creation FAILED\\n",
                  sizeLabel, doStore ? "store" : "probe",
                  (unsigned)workerCount);
    return;
  }

  const uint64_t wallStartUs = esp_timer_get_time();
  __atomic_store_n(&sharedMemoryBenchStart, 1, __ATOMIC_RELEASE);
  for (uint32_t i = 0; i < workerCount; ++i)
    while (!__atomic_load_n(&workers[i].done, __ATOMIC_ACQUIRE)) taskYIELD();
  const uint64_t wallUs = esp_timer_get_time() - wallStartUs;

  const uint64_t totalOperations = (uint64_t)operations * workerCount;
  const double aggregateMops = wallUs
    ? (double)totalOperations / (double)wallUs : 0.0;

  Serial.printf("MEMSHARED %-7s mode=%-5s cores=%u ops/core=%u wall=%" PRIu64
                " us aggregate=%6.3f Mops/s",
                sizeLabel, doStore ? "store" : "probe",
                (unsigned)workerCount, (unsigned)operations,
                wallUs, aggregateMops);
  for (uint32_t i = 0; i < workerCount; ++i) {
    const double nsPerOp = operations
      ? (double)workers[i].elapsedUs * 1000.0 / operations : 0.0;
    Serial.printf(" core%u=%6.1f ns/op",
                  (unsigned)workers[i].core, nsPerOp);
    memoryBenchSink ^= workers[i].checksum;
  }
  Serial.println();
}

static void runSharedPsramCase(const char *label, size_t bytes) {
  TTBenchEntry *entries = (TTBenchEntry *)heap_caps_malloc(
    bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!entries) {
    Serial.printf("MEMSHARED %s: allocation FAILED (%u bytes), free PSRAM=%u\\n",
                  label, (unsigned)bytes, (unsigned)ESP.getFreePsram());
    return;
  }

  const uint32_t entryCount = (uint32_t)(bytes / sizeof(TTBenchEntry));
  Serial.printf("\\nMEMSHARED TABLE %s bytes=%u entries=%u free_after_alloc=%u\\n",
                label, (unsigned)bytes, (unsigned)entryCount,
                (unsigned)ESP.getFreePsram());

  // Initialize once before any worker starts. The two workers then operate
  // on the same allocated table, with independent PRNG streams.
  for (uint32_t i = 0; i < entryCount; ++i) {
    entries[i].key = 0xA5A50000u ^ i;
    entries[i].value = i * 17u;
    entries[i].meta = i ^ 0x5A5A5A5Au;
    entries[i].pad = 0;
  }

  runSharedMemoryMode(entries, entryCount, label, false, 1);
  runSharedMemoryMode(entries, entryCount, label, false, 2);
  runSharedMemoryMode(entries, entryCount, label, true, 1);
  runSharedMemoryMode(entries, entryCount, label, true, 2);

  heap_caps_free(entries);
  Serial.printf("MEMSHARED TABLE %s released; free PSRAM=%u\\n",
                label, (unsigned)ESP.getFreePsram());
}

static void memoryAccessBenchmark() {
  Serial.println("\n=== MEMORY ACCESS BENCHMARK (single core, synthetic 16-byte entries) ===");
  Serial.println("Not Stockfish NPS; no other benchmark task runs concurrently.");
  const uint32_t ops = 300000;
  // Equal 128 KiB working sets give a direct SRAM-vs-PSRAM comparison.
  runMemoryCase("internal_128K", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                128 * 1024, ops, 16);
  runMemoryCase("psram_128K", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                128 * 1024, ops, 16);
  // Larger PSRAM working set shows the effect of a table that exceeds
  // the internal SRAM budget and is less cache-friendly.
  runMemoryCase("psram_1M", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                1024 * 1024, ops, 4);
  Serial.printf("MEMBENCH sink=%08" PRIx32 "\n", (uint32_t)memoryBenchSink);

  Serial.println("\n=== LARGE SHARED PSRAM TABLE TEST (single/dual core) ===");
  Serial.println("Shared table; independent PRNG streams; atomic 32-bit fields.");
  runSharedPsramCase("4MiB", 4 * 1024 * 1024);
  runSharedPsramCase("7MiB", 7 * 1024 * 1024);
  Serial.printf("MEMSHARED sink=%08" PRIx32 "\n", (uint32_t)memoryBenchSink);
}

static volatile bool cpuGo = false;
static volatile uint32_t cpuIterations[2] = {0,0};
static volatile bool cpuDone[2] = {false,false};
static void cpuWorker(void *arg) {
  int index = (int)(intptr_t)arg; uint32_t v = 0x1234567u + index;
  while (!cpuGo) taskYIELD();
  uint32_t start = millis(), count = 0;
  while ((uint32_t)(millis()-start) < 2000) {
    v = v * 1664525u + 1013904223u; v ^= v >> 13; v *= 0x5bd1e995u; ++count;
  }
  cpuIterations[index] = count; cpuDone[index] = true; vTaskDelete(nullptr);
}
static void cpuReport() {
  Serial.println("\n=== DUAL CORE CPU TEST (2 seconds simultaneous) ===");
  cpuGo=false; cpuDone[0]=cpuDone[1]=false; cpuIterations[0]=cpuIterations[1]=0;
  TaskHandle_t t0=nullptr, t1=nullptr;
  BaseType_t a=xTaskCreatePinnedToCore(cpuWorker,"cpu_core0",4096,(void*)0,1,&t0,0);
  BaseType_t b=xTaskCreatePinnedToCore(cpuWorker,"cpu_core1",4096,(void*)1,1,&t1,1);
  if (a!=pdPASS || b!=pdPASS) {
    Serial.printf("Task creation failed: core0=%d core1=%d\n",(int)a,(int)b);
    if(t0) vTaskDelete(t0); if(t1) vTaskDelete(t1); return;
  }
  cpuGo=true; while(!cpuDone[0] || !cpuDone[1]) delay(10);
  Serial.printf("Core 0 iterations: %" PRIu32 "\nCore 1 iterations: %" PRIu32 "\n",
    cpuIterations[0],cpuIterations[1]);
}
static void hardwareReport() {
  esp_chip_info_t chip; esp_chip_info(&chip); uint32_t flashSize=0; esp_flash_get_size(nullptr,&flashSize);
  Serial.println("\n=== CHIP / FLASH ===");
  Serial.printf("Model: %s\nRevision: %u\nCPU cores: %u\nCPU frequency: %u MHz\nSDK: %s\n",
    ESP.getChipModel(),(unsigned)chip.revision,(unsigned)chip.cores,
    (unsigned)ESP.getCpuFreqMHz(),ESP.getSdkVersion());
  Serial.printf("Flash ESP: %u bytes\nFlash IDF: %u bytes\nFlash speed: %u Hz\nFlash mode: %d\n",
    (unsigned)ESP.getFlashChipSize(),(unsigned)flashSize,
    (unsigned)ESP.getFlashChipSpeed(),(int)ESP.getFlashChipMode());
  Serial.printf("PSRAM detected: %s\nFreeRTOS tick rate: %u Hz\n",
    psramFound()?"yes":"NO",(unsigned)configTICK_RATE_HZ);
}
static void drawDisplayClockPattern(uint32_t hz) {
  const int w = tft.width();
  const int h = tft.height();
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, w / 2, h / 2, TFT_RED);
  tft.fillRect(w / 2, 0, w - w / 2, h / 2, TFT_GREEN);
  tft.fillRect(0, h / 2, w / 2, h - h / 2, TFT_BLUE);
  tft.fillRect(w / 2, h / 2, w - w / 2, h - h / 2, TFT_WHITE);
  tft.drawRect(2, 2, w - 4, h - 4, TFT_YELLOW);
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setCursor(w / 2 + 8, h / 2 + 8);
  tft.printf("SPI %u MHz", (unsigned)(hz / 1000000));
}

static void displayBenchmark() {
  Serial.println("\n=== DISPLAY SPI CLOCK BENCHMARK ===");
  Serial.println("Requested clocks: 40 MHz baseline, 80 MHz test, 40 MHz restore.");
  Serial.println("Watch the test pattern at each clock for pixel corruption/flicker.");

  const uint32_t frequencies[] = {40000000, 80000000, 40000000};
  const char *labels[] = {"baseline", "high", "restore"};
  const uint32_t fullRepeats = 8;
  const uint32_t partialRepeats = 24;
  const uint32_t colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE,
                             TFT_BLACK, TFT_CYAN, TFT_MAGENTA, TFT_YELLOW};

  for (unsigned f = 0; f < 3; ++f) {
    const uint32_t hz = frequencies[f];
    tft.setWriteClock(hz);
    delay(30);

    // Full-screen RGB565 transfers.
    uint64_t t0 = esp_timer_get_time();
    for (uint32_t i = 0; i < fullRepeats; ++i)
      tft.fillScreen(colors[i]);
    const uint64_t fullUs = esp_timer_get_time() - t0;

    // A 160x120 rectangle represents a typical partial UI update.
    const int rw = tft.width() / 2;
    const int rh = tft.height() / 2;
    t0 = esp_timer_get_time();
    for (uint32_t i = 0; i < partialRepeats; ++i)
      tft.fillRect(0, 0, rw, rh, colors[i & 7]);
    const uint64_t partialUs = esp_timer_get_time() - t0;

    const uint64_t fullBytes =
      (uint64_t)tft.width() * tft.height() * 2u * fullRepeats;
    const uint64_t partialBytes =
      (uint64_t)rw * rh * 2u * partialRepeats;
    const double fullMiBs = fullUs
      ? ((double)fullBytes / 1048576.0) * 1000000.0 / fullUs : 0.0;
    const double partialMiBs = partialUs
      ? ((double)partialBytes / 1048576.0) * 1000000.0 / partialUs : 0.0;

    Serial.printf("DISPLAYCLOCK %-8s requested=%u MHz full=%" PRIu64
                  " us/frame=%.1f us (%6.2f MiB/s) partial=%" PRIu64
                  " us/op=%.1f us (%6.2f MiB/s)\n",
                  labels[f], (unsigned)(hz / 1000000),
                  fullUs, (double)fullUs / fullRepeats, fullMiBs,
                  partialUs, (double)partialUs / partialRepeats,
                  partialMiBs);

    drawDisplayClockPattern(hz);
    delay(1500); // Allows a visual stability check at this clock.
  }

  tft.setWriteClock(40000000);
  Serial.println("DISPLAYCLOCK restored to 40 MHz (known-good baseline).");
}
static void drawDashboard() {
  tft.fillScreen(TFT_BLACK); tft.setTextColor(TFT_WHITE,TFT_BLACK);
  tft.setTextSize(2); tft.setCursor(8,8); tft.println("Stockfish ESP32");
  tft.setTextSize(1); tft.setTextColor(TFT_CYAN,TFT_BLACK);
  tft.println("Hardware diagnostic / FNK0104B");
  tft.drawFastHLine(8,48,tft.width()-16,TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE,TFT_BLACK); tft.setCursor(8,58);
  tft.printf("Chip: %s",ESP.getChipModel());
  tft.setCursor(8,74); tft.printf("CPU: %u MHz / %u cores",(unsigned)ESP.getCpuFreqMHz(),(unsigned)ESP.getChipCores());
  tft.setCursor(8,90); tft.printf("Flash: %u MiB",(unsigned)(ESP.getFlashChipSize()/(1024*1024)));
  tft.setCursor(8,106); tft.printf("PSRAM: %u MiB (%s)",(unsigned)(ESP.getPsramSize()/(1024*1024)),psramFound()?"OK":"MISSING");
  tft.setCursor(8,122); tft.printf("Free heap: %u KiB",(unsigned)(ESP.getFreeHeap()/1024));
  tft.setCursor(8,138); tft.printf("Free PSRAM: %u KiB",(unsigned)(ESP.getFreePsram()/1024));
  tft.drawFastHLine(8,158,tft.width()-16,TFT_DARKGREY);
  tft.setCursor(8,170); tft.setTextColor(TFT_YELLOW,TFT_BLACK); tft.println("Touch the screen:");
  tft.setTextColor(TFT_WHITE,TFT_BLACK); tft.setCursor(8,188); tft.println("Touch: waiting");
  tft.setCursor(8,204); tft.setTextColor(TFT_GREEN,TFT_BLACK); tft.println("Serial: 115200 baud");
  const uint32_t bars[]={TFT_RED,TFT_GREEN,TFT_BLUE,TFT_CYAN,TFT_MAGENTA,TFT_YELLOW};
  int y=tft.height()-22,w=tft.width()/6;
  for(int i=0;i<6;++i) tft.fillRect(i*w,y,w,22,bars[i]);
}
void setup() {
  Serial.begin(115200); delay(1200);
  Serial.println("\n=== Stockfish ESP32-S3 Board Diagnostics / FNK0104B ===");
  pinMode(LCD_BL,OUTPUT); digitalWrite(LCD_BL,HIGH);
  tft.init(); tft.setRotation(1);
  Serial.printf("Display initialized: %d x %d\n",tft.width(),tft.height());
  pinMode(TOUCH_RST,OUTPUT); digitalWrite(TOUCH_RST,HIGH);
  Wire.begin(TOUCH_SDA,TOUCH_SCL,400000); delay(50);
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  bool touchPresent=(Wire.endTransmission()==0);
  Serial.printf("FT6336U I2C @0x38: %s\n",touchPresent?"ACK":"NOT FOUND");
  hardwareReport(); memoryReport(); memoryAccessBenchmark(); displayBenchmark(); cpuReport();
  Serial.println("\n=== READY: touch panel; copy complete serial log ===");
  drawDashboard();
}
void loop() {
  static uint32_t lastTouch=0; uint16_t x=0,y=0; uint8_t points=0;
  if(touchRead(x,y,points) && millis()-lastTouch>100) {
    lastTouch=millis(); Serial.printf("TOUCH points=%u x=%u y=%u\n",points,x,y);
    tft.fillRect(8,184,tft.width()-16,18,TFT_BLACK);
    tft.setCursor(8,188); tft.setTextColor(TFT_GREEN,TFT_BLACK);
    tft.printf("Touch: %u, %u",x,y);
  }
  delay(10);
}
