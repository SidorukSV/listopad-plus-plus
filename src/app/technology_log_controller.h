#pragma once

#include "listopad/file_io.h"
#include "listopad/technology_log.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <thread>

namespace listopad::app {

struct TechnologyLogLoadResult {
  std::uint64_t generation{0};
  unsigned long error{0};
  MappedFile file;
  TechnologyLogDocument document;
};

class TechnologyLogController final {
 public:
  TechnologyLogController() = default;
  ~TechnologyLogController();
  TechnologyLogController(const TechnologyLogController&) = delete;
  TechnologyLogController& operator=(const TechnologyLogController&) = delete;

  void cancel() noexcept;
  void start(HWND destination, UINT result_message,
             std::filesystem::path path);

  [[nodiscard]] bool accepts(
      const TechnologyLogLoadResult& result) const noexcept {
    return result.generation == generation_;
  }

 private:
  std::jthread worker_;
  std::uint64_t generation_{0};
};

}  // namespace listopad::app
