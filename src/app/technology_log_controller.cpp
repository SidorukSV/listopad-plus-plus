#include "technology_log_controller.h"

#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace listopad::app {
namespace {

void post_result(const HWND destination, const UINT message,
                 std::unique_ptr<TechnologyLogLoadResult> result) {
  TechnologyLogLoadResult* raw = result.release();
  if (!PostMessageW(destination, message, 0,
                    reinterpret_cast<LPARAM>(raw))) {
    delete raw;
  }
}

}  // namespace

TechnologyLogController::~TechnologyLogController() {
  worker_.request_stop();
}

void TechnologyLogController::cancel() noexcept {
  worker_.request_stop();
  ++generation_;
}

void TechnologyLogController::start(
    const HWND destination, const UINT result_message,
    std::filesystem::path path) {
  worker_.request_stop();
  const std::uint64_t generation = ++generation_;
  worker_ = std::jthread(
      [destination, result_message, generation,
       path = std::move(path)](const std::stop_token stop) {
        auto completed = std::make_unique<TechnologyLogLoadResult>();
        completed->generation = generation;
        if (!completed->file.open(path)) {
          completed->error = completed->file.error();
          if (!stop.stop_requested()) {
            post_result(destination, result_message, std::move(completed));
          }
          return;
        }
        if (completed->file.size() >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
          completed->error = ERROR_FILE_TOO_LARGE;
          if (!stop.stop_requested()) {
            post_result(destination, result_message, std::move(completed));
          }
          return;
        }
        std::string_view source;
        if (completed->file.size() != 0) {
          source = {
              reinterpret_cast<const char*>(completed->file.data()),
              static_cast<std::size_t>(completed->file.size()),
          };
        }
        completed->document = parse_technology_log(
            source, path.filename().string(), stop);
        if (stop.stop_requested() || completed->document.cancelled) return;
        post_result(destination, result_message, std::move(completed));
      });
}

}  // namespace listopad::app
