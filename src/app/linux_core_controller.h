#pragma once

#include "listopad/linux_core_dump.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <thread>

namespace listopad::app {

struct LinuxCoreLoadResult {
  std::uint64_t generation{0};
  LinuxCoreReadResult parsed;
};

class LinuxCoreController final {
 public:
  LinuxCoreController() = default;
  ~LinuxCoreController();
  LinuxCoreController(const LinuxCoreController&) = delete;
  LinuxCoreController& operator=(const LinuxCoreController&) = delete;

  void cancel() noexcept;
  void start(HWND destination, UINT result_message,
             std::filesystem::path path);

  [[nodiscard]] bool accepts(
      const LinuxCoreLoadResult& result) const noexcept {
    return result.generation == generation_;
  }

 private:
  std::jthread worker_;
  std::uint64_t generation_{0};
};

}  // namespace listopad::app
