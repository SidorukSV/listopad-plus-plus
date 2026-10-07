#include "pff_profile_controller.h"

#include "listopad/file_io.h"

#include <memory>
#include <string_view>
#include <utility>

namespace listopad::app {
namespace {

constexpr std::uint64_t kMaxPffBytes = 512ull * 1024ull * 1024ull;

void post_result(const HWND destination, const UINT message,
                 std::unique_ptr<PffProfileLoadResult> result) {
  PffProfileLoadResult* raw = result.release();
  if (!PostMessageW(destination, message, 0,
                    reinterpret_cast<LPARAM>(raw))) {
    delete raw;
  }
}

}  // namespace

PffProfileController::~PffProfileController() {
  worker_.request_stop();
}

void PffProfileController::cancel() noexcept {
  worker_.request_stop();
  ++generation_;
}

void PffProfileController::start(const HWND destination,
                                 const UINT result_message,
                                 std::filesystem::path path) {
  worker_.request_stop();
  const std::uint64_t generation = ++generation_;
  worker_ = std::jthread(
      [destination, result_message, generation,
       path = std::move(path)](const std::stop_token stop) {
        auto completed = std::make_unique<PffProfileLoadResult>();
        completed->generation = generation;
        MappedFile file;
        if (!file.open(path)) {
          completed->error = file.error();
        } else if (file.size() > kMaxPffBytes) {
          completed->error = ERROR_FILE_TOO_LARGE;
        } else {
          std::string_view source;
          if (file.size() != 0 && file.data()) {
            source = {reinterpret_cast<const char*>(file.data()),
                      static_cast<std::size_t>(file.size())};
          }
          completed->document = parse_pff_profile(source, stop);
          if (completed->document.records.empty() &&
              !completed->document.cancelled) {
            completed->error = ERROR_INVALID_DATA;
          }
        }
        if (stop.stop_requested() || completed->document.cancelled) return;
        post_result(destination, result_message, std::move(completed));
      });
}

}  // namespace listopad::app
