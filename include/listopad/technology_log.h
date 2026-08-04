#pragma once

#include "listopad/ui_layout.h"

#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace listopad {

struct TechnologyLogTextRange {
  std::size_t offset{0};
  std::size_t length{0};
};

struct TechnologyLogField {
  TechnologyLogTextRange name;
  TechnologyLogTextRange value;
  char quote{'\0'};
};

struct TechnologyLogEvent {
  TechnologyLogTextRange raw;
  int minute{0};
  int second{0};
  int microsecond{0};
  std::uint64_t duration_us{0};
  std::string type;
  int nesting_level{0};
  std::vector<TechnologyLogField> fields;
  bool incomplete{false};
};

struct TechnologyLogFileTime {
  int year{0};
  int month{0};
  int day{0};
  int hour{0};
  bool valid{false};
};

struct TechnologyLogDocument {
  TechnologyLogFileTime file_time;
  std::vector<TechnologyLogEvent> events;
  std::size_t skipped_bytes{0};
  bool cancelled{false};
};

enum class TechnologyLogInterpretationKind {
  Information,
  Warning,
  Error,
};

struct TechnologyLogInterpretation {
  TechnologyLogInterpretationKind kind{
      TechnologyLogInterpretationKind::Information};
  std::string summary;
};

enum class TechnologyLogFilter {
  All,
  Warnings,
  Slow,
  Http,
  Cache,
};

struct TechnologyLogUiState {
  TechnologyLogFilter filter{TechnologyLogFilter::All};
  std::size_t selected_event{0};
  bool show_raw{false};
};

[[nodiscard]] bool looks_like_technology_log(
    std::string_view source) noexcept;
[[nodiscard]] TechnologyLogDocument parse_technology_log(
    std::string_view source, std::string_view file_name,
    std::stop_token stop = {});
[[nodiscard]] std::string_view technology_log_raw_event(
    std::string_view source, const TechnologyLogEvent& event) noexcept;
[[nodiscard]] std::string_view technology_log_field_name(
    std::string_view source, const TechnologyLogField& field) noexcept;
[[nodiscard]] std::string technology_log_field_value(
    std::string_view source, const TechnologyLogEvent& event,
    std::string_view name);
[[nodiscard]] std::string technology_log_format_timestamp(
    const TechnologyLogDocument& document,
    const TechnologyLogEvent& event);
[[nodiscard]] std::string technology_log_format_duration(
    std::uint64_t duration_us, bool russian);
[[nodiscard]] TechnologyLogInterpretation interpret_technology_log_event(
    std::string_view source, const TechnologyLogEvent& event, bool russian);

struct TechnologyLogLayout {
  LayoutRect summary;
  LayoutRect filter;
  LayoutRect events;
  LayoutRect raw_toggle;
  LayoutRect details;
  LayoutRect raw;
};

[[nodiscard]] TechnologyLogLayout calculate_technology_log_layout(
    int width, int height, int dpi, bool show_raw) noexcept;

}  // namespace listopad
