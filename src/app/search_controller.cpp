#include "search_controller.h"

#include <memory>
#include <utility>

namespace listopad::app {
namespace {

void post_result(const HWND destination, const UINT result_message,
                 std::unique_ptr<SearchJobResult> result) {
  SearchJobResult* raw = result.release();
  if (!PostMessageW(destination, result_message, 0,
                    reinterpret_cast<LPARAM>(raw))) {
    delete raw;
  }
}

}  // namespace

SearchController::~SearchController() { worker_.request_stop(); }

void SearchController::cancel() noexcept {
  worker_.request_stop();
  ++generation_;
}

std::uint64_t SearchController::begin_job() {
  worker_.request_stop();
  return ++generation_;
}

void SearchController::start_find(
    const HWND destination, const UINT result_message,
    const SearchJobKind kind, std::vector<SearchTabResult> snapshots,
    std::string pattern, const SearchOptions options, const bool wrap) {
  const std::uint64_t generation = begin_job();
  worker_ = std::jthread(
      [destination, result_message, generation, kind, wrap, options,
       pattern = std::move(pattern),
       snapshots = std::move(snapshots)](const std::stop_token stop) mutable {
        auto completed = std::make_unique<SearchJobResult>();
        completed->kind = kind;
        completed->generation = generation;
        completed->wrap = wrap;
        completed->tabs = std::move(snapshots);
        for (auto& item : completed->tabs) {
          item.found = search_all(item.subject, pattern, options, stop);
          if (stop.stop_requested()) return;
        }
        post_result(destination, result_message, std::move(completed));
      });
}

void SearchController::start_replace_all(
    const HWND destination, const UINT result_message,
    std::vector<SearchTabResult> snapshots, std::string pattern,
    std::string replacement, const SearchOptions options) {
  const std::uint64_t generation = begin_job();
  worker_ = std::jthread(
      [destination, result_message, generation, options,
       pattern = std::move(pattern), replacement = std::move(replacement),
       snapshots = std::move(snapshots)](const std::stop_token stop) mutable {
        auto completed = std::make_unique<SearchJobResult>();
        completed->kind = SearchJobKind::ReplaceAll;
        completed->generation = generation;
        completed->tabs = std::move(snapshots);
        for (auto& item : completed->tabs) {
          item.replaced =
              replace_all(item.subject, pattern, replacement, options, stop);
          if (stop.stop_requested()) return;
        }
        post_result(destination, result_message, std::move(completed));
      });
}

}  // namespace listopad::app
