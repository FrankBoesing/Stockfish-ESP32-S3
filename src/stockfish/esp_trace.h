#ifndef STOCKFISH_ESP_TRACE_H_INCLUDED
#define STOCKFISH_ESP_TRACE_H_INCLUDED

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
// Keep Arduino.h out of Stockfish headers to avoid leaking GPIO macros.
// Detailed traces remain optional and are enabled by CORE_DEBUG_LEVEL=4/5.
#if defined(CORE_DEBUG_LEVEL) && CORE_DEBUG_LEVEL >= 4
void stockfish_esp_trace(const char* format, ...);
#define SF_ESP_TRACE(...) stockfish_esp_trace(__VA_ARGS__)
#else
#define SF_ESP_TRACE(...) ((void) 0)
#endif
#else
#define SF_ESP_TRACE(...) ((void) 0)
#endif

#endif  // STOCKFISH_ESP_TRACE_H_INCLUDED
