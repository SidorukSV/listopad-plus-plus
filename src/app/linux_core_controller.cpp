#include "linux_core_controller.h"

#include <memory>
#include <utility>

namespace listopad::app {
namespace {

void post_result(const HWND destination, const UINT message,
                 std::unique_ptr<LinuxCoreLoadResult> result) {
  LinuxCoreLoadResult* raw = result.release();
  if (!PostMessageW(destination, message, 0,
                    reinterpret_cast<LPARAM>(raw))) {
    delete raw;
  }
}

}  // namespace

LinuxCoreController::~LinuxCoreController() {
  worker_.request_stop();
}

void LinuxCoreController::cancel() noexcept {
  worker_.request_stop();
  ++generation_;
}

void LinuxCoreController::start(const HWND destination,
                                const UINT result_message,
                                std::filesystem::path path) {
  worker_.request_stop();
  const std::uint64_t generation = ++generation_;
  worker_ = std::jthread(
      [destination, result_message, generation,
       path = std::move(path)](const std::stop_token stop) {
        auto completed = std::make_unique<LinuxCoreLoadResult>();
        completed->generation = generation;
        completed->parsed = read_linux_core_file(path, stop);
        if (stop.stop_requested() ||
            completed->parsed.error == LinuxCoreError::Cancelled) {
          return;
        }
        post_result(destination, result_message, std::move(completed));
      });
}

}  // namespace listopad::app
