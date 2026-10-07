#pragma once

#include "listopad/ui_layout.h"

#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace listopad {

struct PffProfileRecord {
  std::string extension;
  std::string module;
  std::uint32_t line{0};
  std::string code;
  std::uint64_t calls{0};
  double inclusive_time{0.0};
  double self_time{0.0};
  double inclusive_share{0.0};
  double self_share{0.0};
  bool client{false};
  bool server{false};
  bool server_call{false};
  bool incomplete{false};
};

struct PffProfileDocument {
  std::vector<PffProfileRecord> records;
  std::size_t module_count{0};
  std::size_t skipped_records{0};
  double total_self_time{0.0};
  bool cancelled{false};
};

enum class PffProfileFilter {
  All,
  Hot,
  Client,
  Server,
  ServerCalls,
};

struct PffProfileUiState {
  PffProfileFilter filter{PffProfileFilter::All};
  std::size_t selected_record{0};
  int sort_column{5};
  bool sort_descending{true};
};

[[nodiscard]] bool looks_like_pff_profile(std::string_view source) noexcept;
[[nodiscard]] PffProfileDocument parse_pff_profile(
    std::string_view source, std::stop_token stop = {});
[[nodiscard]] bool pff_profile_record_matches_filter(
    const PffProfileRecord& record, PffProfileFilter filter) noexcept;
[[nodiscard]] std::string pff_profile_format_time(double seconds,
                                                  bool russian);
[[nodiscard]] std::string pff_profile_format_share(double share,
                                                   bool russian);
[[nodiscard]] std::string pff_profile_context(
    const PffProfileRecord& record, bool russian);

struct PffProfileLayout {
  LayoutRect summary;
  LayoutRect filter;
  LayoutRect records;
};

[[nodiscard]] PffProfileLayout calculate_pff_profile_layout(
    int width, int height, int dpi) noexcept;

}  // namespace listopad
