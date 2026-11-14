#include <Arduino.h>
#include <cstdarg>
#include <cstdio>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Shared by the diagnostic probe and the interactive GUI application.
// Keep this translation unit independent of Stockfish headers to avoid
// leaking Arduino GPIO macros into Stockfish source files.
void stockfish_esp_trace(const char *format, ...) {
  char buffer[256];

  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  const char *taskName = pcTaskGetName(nullptr);
  const UBaseType_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
  Serial.printf("[SFTRACE][%s stack-free~%u] %s\n",
                taskName ? taskName : "?",
                (unsigned)stackFree,
                buffer);
  Serial.flush();
}
