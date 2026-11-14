#ifndef STOCKFISH_ESP_TRACE_H_INCLUDED
#define STOCKFISH_ESP_TRACE_H_INCLUDED

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
// Implemented by the ESP32 logging layer. Keeping Arduino.h out of Stockfish
// headers avoids leaking Arduino macros (notably cli()) into Stockfish files.
// CORE_DEBUG_LEVEL=4 (DEBUG) or 5 (VERBOSE) enables these optional traces;
// lower levels compile the calls out completely.
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
