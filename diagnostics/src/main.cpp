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
  size_t testSize = largest > 65536 ? min(largest - 32768, (size_t)(2 * 1024 * 1024)) : 0;
  Serial.printf("PSRAM largest block: %u bytes; test allocation: %u bytes\n",
    (unsigned)largest, (unsigned)testSize);
  if (!testSize) return;
  uint8_t *mem = (uint8_t *)heap_caps_malloc(testSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
static void displayBenchmark() {
  Serial.println("\n=== DISPLAY BENCHMARK ===");
  const uint32_t colors[]={TFT_RED,TFT_GREEN,TFT_BLUE,TFT_WHITE,TFT_BLACK};
  const char *names[]={"red","green","blue","white","black"};
  for(unsigned i=0;i<5;++i) {
    uint32_t t=micros(); tft.fillScreen(colors[i]);
    Serial.printf("fillScreen %-5s: %u us\n",names[i],(unsigned)(micros()-t)); delay(250);
  }
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
  hardwareReport(); memoryReport(); displayBenchmark(); cpuReport();
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
