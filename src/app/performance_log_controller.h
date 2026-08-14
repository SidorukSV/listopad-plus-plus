#pragma once

#include "listopad/performance_log.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <thread>

namespace listopad::app {

struct PerformanceLogLoadResult {
  std::uint64_t generation{0};
  unsigned long error{0};
  // A binary log fails with a PDH status code, which FormatMessage can only
  // resolve against pdh.dll; a text log fails with an ordinary Win32 code.
  bool pdh_status{false};
  PerformanceLogDocument document;
};

class PerformanceLogController final {
 public:
  PerformanceLogController() = default;
  ~PerformanceLogController();
  PerformanceLogController(const PerformanceLogController&) = delete;
  PerformanceLogController& operator=(const PerformanceLogController&) =
      delete;

  void cancel() noexcept;
  void start(HWND destination, UINT result_message,
             std::filesystem::path path);

  [[nodiscard]] bool accepts(
      const PerformanceLogLoadResult& result) const noexcept {
    return result.generation == generation_;
  }

 private:
  std::jthread worker_;
  std::uint64_t generation_{0};
};

// Resolves a PDH status through pdh.dll; ordinary Win32 codes keep using
// win32_error_message.
[[nodiscard]] std::wstring pdh_status_message(unsigned long status);

}  // namespace listopad::app
