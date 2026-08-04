#include "listopad/document_map_cache.h"
#include "listopad/search.h"
#include "listopad/session.h"
#include "listopad/ui_layout.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;

class TemporaryProfile final {
 public:
  TemporaryProfile() {
    std::wstring buffer(32768, L'\0');
    const DWORD length =
        GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    buffer.resize(length);
    path_ = std::filesystem::path(buffer) /
            (L"ListopadPP-benchmark-" +
             std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryProfile() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

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

bool benchmark_session_manifest() {
  TemporaryProfile profile;
  listopad::SessionStore store(profile.path());
  listopad::SessionManifest manifest;
  manifest.clean_shutdown = true;
  manifest.active_index = 77;
  for (int index = 0; index < 100; ++index) {
    listopad::SessionTab tab;
    tab.id = "00000000-0000-0000-0000-" +
             std::string(12 - std::to_string(index).size(), '0') +
             std::to_string(index);
    tab.path = profile.path() /
               (L"document-" + std::to_wstring(index) + L".txt");
    tab.title = L"Document " + std::to_wstring(index);
    tab.caret = index * 1000;
    tab.first_visible_line = index * 10;
    manifest.tabs.push_back(std::move(tab));
  }
  if (!store.save_manifest(manifest)) return false;
  const auto loaded = store.load_manifest();
  return loaded.status == listopad::StoreLoadStatus::Loaded &&
         loaded.value.tabs.size() == 100 &&
         loaded.value.active_index == 77;
}

bool benchmark_recovery_snapshot() {
  TemporaryProfile profile;
  listopad::SessionStore store(profile.path());
  listopad::RecoverySnapshot snapshot;
  snapshot.id = "01234567-89ab-cdef-0123-456789abcdef";
  snapshot.title = L"16 MiB recovery";
  snapshot.content.assign(16U * 1024U * 1024U, 'x');
  if (!store.save_recovery(snapshot)) return false;
  const auto loaded = store.load_recovery(
      store.recovery_path(snapshot.id));
  return loaded.status == listopad::StoreLoadStatus::Loaded &&
         loaded.value.content == snapshot.content;
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
  passed &= run_budget("session_manifest_100_tabs", 250.0,
                       benchmark_session_manifest);
  passed &= run_budget("recovery_snapshot_16_mib", 1500.0,
                       benchmark_recovery_snapshot);
  return passed ? 0 : 1;
}
