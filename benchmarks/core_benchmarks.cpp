#include "listopad/document_map_cache.h"
#include "listopad/search.h"
#include "listopad/ui_layout.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using Clock = std::chrono::steady_clock;

template <typename Operation>
bool run_budget(const std::string_view name, const double budget_ms,
                Operation&& operation) {
  const auto started = Clock::now();
  const bool result_is_valid = operation();
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  const bool passed = result_is_valid && elapsed_ms <= budget_ms;

  std::cout << "{\"benchmark\":\"" << name << "\",\"elapsed_ms\":"
            << std::fixed << std::setprecision(3) << elapsed_ms
            << ",\"budget_ms\":" << budget_ms << ",\"status\":\""
            << (passed ? "pass" : "fail") << "\"}\n";
  return passed;
}

bool benchmark_large_document_search() {
  constexpr std::size_t kDocumentBytes = 16U * 1024U * 1024U;
  constexpr std::string_view kNeedle = "listopad-large-document-target";
  std::string document(kDocumentBytes, 'x');
  document.replace(document.size() - kNeedle.size(), kNeedle.size(), kNeedle);

  const auto result =
      listopad::search_all(document, kNeedle, listopad::SearchOptions{});
  return result.ok && result.matches.size() == 1 &&
         result.matches.front().start == document.size() - kNeedle.size();
}

bool benchmark_sparse_document_map_cache() {
  constexpr std::size_t kLogicalLines = 10'000'000;
  constexpr std::size_t kCachedLines = 10'000;
  constexpr std::size_t kStride = 997;

  listopad::DocumentMapLineCache cache;
  cache.reset(kLogicalLines);
  for (std::size_t index = 0; index < kCachedLines; ++index) {
    cache.store(index * kStride, listopad::DocumentMapLine{{0, 72, 1}});
  }

  const auto* last = cache.line((kCachedLines - 1) * kStride);
  return cache.line_count() == kLogicalLines &&
         cache.cached_line_count() == kCachedLines && last != nullptr &&
         *last == listopad::DocumentMapLine{{0, 72, 1}} &&
         cache.line(1) == nullptr;
}

bool benchmark_layout_recalculation() {
  constexpr int kIterations = 500'000;
  std::uint64_t checksum = 0;
  listopad::WindowLayoutInput input{
      .client_width = 3840,
      .client_height = 2160,
      .status_height = 28,
      .toolbar_height = 36,
      .dpi = 192,
  };
  const listopad::UiState state{
      .banner_visible = true,
      .search_mode = listopad::SearchMode::Replace,
      .search_results_visible = true,
  };

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    input.client_width = 1920 + iteration % 1921;
    const auto window = listopad::calculate_window_layout(input, state);
    const auto panes = listopad::calculate_editor_pane_layout(
        {0, 0, input.client_width, window.search_results.y}, true, input.dpi);
    checksum += static_cast<std::uint64_t>(
        window.find_text.width + window.search_results.height +
        panes.editor.width + panes.document_map.width);
  }
  std::cout << "{\"layout_checksum\":" << checksum << "}\n";
  return checksum != 0;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= run_budget("search_all_16_mib", 750.0,
                       benchmark_large_document_search);
  passed &= run_budget("document_map_10m_lines_sparse_cache", 250.0,
                       benchmark_sparse_document_map_cache);
  passed &= run_budget("layout_500k_recalculations", 500.0,
                       benchmark_layout_recalculation);
  return passed ? 0 : 1;
}
