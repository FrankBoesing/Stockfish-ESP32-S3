#include "esp_fatal.h"

#include <cstdlib>

#if defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)

    #include <Arduino.h>
    #include <esp_debug_helpers.h>
    #include <esp_heap_caps.h>
    #include <freertos/FreeRTOS.h>
    #include <freertos/task.h>

    #include <cstdint>

namespace Stockfish {
namespace {

constexpr unsigned MaxBacktraceFrames = 24;

struct FatalBacktrace {
    esp_backtrace_frame_t frames[MaxBacktraceFrames];
    unsigned count = 0;
    bool corrupted = false;
    bool truncated = false;
};

// Capture before printing anything: Serial output would add its own frames
// and could change the stack while it is being inspected. Suspending the local
// scheduler keeps this task on the current core during the Xtensa stack walk.
static void capture_fatal_backtrace(FatalBacktrace& trace) {
    vTaskSuspendAll();

    esp_backtrace_frame_t frame = {};
    esp_backtrace_get_start(&frame.pc, &frame.sp, &frame.next_pc);
    trace.frames[trace.count++] = frame;

    while (frame.next_pc != 0) {
        if (trace.count >= MaxBacktraceFrames) {
            trace.truncated = true;
            break;
        }

        if (!esp_backtrace_get_next_frame(&frame)) {
            trace.corrupted = true;
            break;
        }

        trace.frames[trace.count++] = frame;
    }

    (void) xTaskResumeAll();
}

[[noreturn]] static void report_fatal(const char* kind,
                                      int status,
                                      const char* file,
                                      int line,
                                      const char* reason) {
    FatalBacktrace trace;
    capture_fatal_backtrace(trace);

    const char* taskName = pcTaskGetName(nullptr);
    const UBaseType_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
    const uint32_t psramFree =
      heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const uint32_t psramLargest =
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const uint32_t internalFree =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    Serial.println();
    Serial.println("========== STOCKFISH FATAL ERROR ==========");
    Serial.printf("Type       : %s", kind);
    if (kind[0] == 'e')
        Serial.printf(" (status %d)", status);
    Serial.println();
    Serial.printf("Reason     : %s\n", reason ? reason : "(unspecified)");
    Serial.printf("Location   : %s:%d\n", file ? file : "(unknown)", line);
    Serial.printf("Task       : %s\n", taskName ? taskName : "(unknown)");
    Serial.printf("Stack free : %u bytes\n", (unsigned) stackFree);
    Serial.printf("PSRAM free : %u bytes; largest block: %u bytes\n",
                  (unsigned) psramFree, (unsigned) psramLargest);
    Serial.printf("Internal free: %u bytes\n", (unsigned) internalFree);

    Serial.printf("Backtrace (%u frames):\n", trace.count);
    for (unsigned i = 0; i < trace.count; ++i) {
        Serial.printf("  #%02u  PC=0x%08x  SP=0x%08x\n",
                      i,
                      (unsigned) trace.frames[i].pc,
                      (unsigned) trace.frames[i].sp);
    }
    if (trace.corrupted)
        Serial.println("  [backtrace stopped: stack frame is corrupt]");
    if (trace.truncated)
        Serial.println("  [backtrace truncated at frame limit]");

    Serial.println("Calling task is suspended; reset the board manually after saving this log.");
    Serial.println("===========================================");
    Serial.flush();

    // Do not turn an ordinary Stockfish error into abort()->panic->reset.
    // Suspending only this task preserves the diagnostic and leaves the board
    // and USB CDC connection alive for inspection.
    for (;;)
        vTaskSuspend(nullptr);
}

}  // namespace

[[noreturn]] void fatal_exit(int status, const char* file, int line, const char* reason) {
    report_fatal("exit", status, file, line, reason);
}

[[noreturn]] void fatal_abort(const char* file, int line, const char* reason) {
    report_fatal("abort", 0, file, line, reason);
}

}  // namespace Stockfish

#else

namespace Stockfish {

[[noreturn]] void fatal_exit(int status, const char*, int, const char*) {
    std::exit(status);
}

[[noreturn]] void fatal_abort(const char*, int, const char*) {
    std::abort();
}

}  // namespace Stockfish

#endif
