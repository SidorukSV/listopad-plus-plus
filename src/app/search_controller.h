#pragma once

#include "listopad/search.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace listopad::app {

enum class SearchJobKind {
  Find,
  FindAll,
  ReplaceAll,
};

struct SearchTabResult {
  int index{-1};
  std::size_t base{0};
  std::size_t caret{0};
  std::string original;
  std::string subject;
  SearchResult found;
  ReplaceResult replaced;
};

struct SearchJobResult {
  SearchJobKind kind{SearchJobKind::Find};
  std::uint64_t generation{0};
  bool wrap{true};
  std::vector<SearchTabResult> tabs;
};

class SearchController final {
 public:
  SearchController() = default;
  ~SearchController();
  SearchController(const SearchController&) = delete;
  SearchController& operator=(const SearchController&) = delete;

  void cancel() noexcept;
  void start_find(HWND destination, UINT result_message, SearchJobKind kind,
                  std::vector<SearchTabResult> snapshots,
                  std::string pattern, SearchOptions options, bool wrap);
  void start_replace_all(HWND destination, UINT result_message,
                         std::vector<SearchTabResult> snapshots,
                         std::string pattern, std::string replacement,
                         SearchOptions options);

  [[nodiscard]] bool accepts(const SearchJobResult& result) const noexcept {
    return result.generation == generation_;
  }

 private:
  [[nodiscard]] std::uint64_t begin_job();

  std::jthread worker_;
  std::uint64_t generation_{0};
};

}  // namespace listopad::app
