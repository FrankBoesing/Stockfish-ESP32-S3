#ifndef STOCKFISH_ESP_FATAL_H_INCLUDED
#define STOCKFISH_ESP_FATAL_H_INCLUDED

namespace Stockfish {

// Preserve normal process termination on desktop platforms. On ESP32 these
// functions print a diagnostic and suspend only the calling FreeRTOS task.
[[noreturn]] void fatal_exit(int status, const char* file, int line, const char* reason);
[[noreturn]] void fatal_abort(const char* file, int line, const char* reason);

}  // namespace Stockfish

#define SF_FATAL_EXIT(status, reason) \
    ::Stockfish::fatal_exit((status), __FILE__, __LINE__, (reason))
#define SF_FATAL_ABORT(reason) \
    ::Stockfish::fatal_abort(__FILE__, __LINE__, (reason))

#endif  // STOCKFISH_ESP_FATAL_H_INCLUDED
