#ifndef LOG_LOCAL_LEVEL
  #if defined(CORE_DEBUG_LEVEL)
    #define LOG_LOCAL_LEVEL CORE_DEBUG_LEVEL
  #else
    #define LOG_LOCAL_LEVEL 3  // ESP_LOG_INFO
  #endif
#endif

#include <cstdarg>
#include <cstdio>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr char Tag[] = "Stockfish";
}

// Keep the Stockfish sources independent of Arduino.h: it defines GPIO
// macros (notably cli()) that conflict with identifiers in Stockfish.
// All optional engine traces go through ESP-IDF logging so their output,
// color and verbosity are controlled by the normal ESP log configuration.
void stockfish_esp_trace(const char *format, ...) {
#if defined(CORE_DEBUG_LEVEL) && CORE_DEBUG_LEVEL >= 4
  // Align this tag's runtime threshold with Arduino's CORE_DEBUG_LEVEL.
  // LOG_LOCAL_LEVEL above also prevents higher levels from entering this TU.
  static const bool logLevelConfigured = []() {
    esp_log_level_set(Tag, static_cast<esp_log_level_t>(CORE_DEBUG_LEVEL));
    return true;
  }();
  (void)logLevelConfigured;

  char buffer[256];

  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  const char *taskName = pcTaskGetName(nullptr);
  const UBaseType_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
  ESP_LOGD(Tag, "[%s stack-free~%u] %s",
           taskName ? taskName : "?",
           (unsigned)stackFree,
           buffer);
#else
  (void)format;
#endif
}

void stockfish_esp_task_info(const char *role, int id) {
  const char *taskName = pcTaskGetName(nullptr);
  const UBaseType_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
  ESP_LOGI(Tag, "TASK role=%s id=%d task=%s core=%d stack-free~%u",
           role ? role : "?",
           id,
           taskName ? taskName : "?",
           (int)xPortGetCoreID(),
           (unsigned)stackFree);
}
