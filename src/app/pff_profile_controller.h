#pragma once

#include "listopad/pff_profile.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <thread>

namespace listopad::app {

struct PffProfileLoadResult {
  std::uint64_t generation{0};
  unsigned long error{0};
  PffProfileDocument document;
};

class PffProfileController final {
 public:
  PffProfileController() = default;
  ~PffProfileController();
  PffProfileController(const PffProfileController&) = delete;
  PffProfileController& operator=(const PffProfileController&) = delete;

  void cancel() noexcept;
  void start(HWND destination, UINT result_message,
             std::filesystem::path path);

  [[nodiscard]] bool accepts(
      const PffProfileLoadResult& result) const noexcept {
    return result.generation == generation_;
  }

 private:
  std::jthread worker_;
  std::uint64_t generation_{0};
};

}  // namespace listopad::app
