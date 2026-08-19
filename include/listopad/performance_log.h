#pragma once

#include "listopad/ui_layout.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace listopad {

// Windows Performance Monitor logs are read as a source-oriented projection:
// every sample keeps the value the log recorded, and a missing sample stays
// missing instead of being interpolated. Rate counters have no value in the
// first sample, and instances that appear later in the run have no value
// before they existed, so gaps are a normal part of a valid log.
inline constexpr double kPerformanceLogNoValue =
    -1.7976931348623157e308;  // lowest finite double, never a counter value

[[nodiscard]] bool performance_log_has_value(double value) noexcept;

enum class PerformanceLogSourceKind {
  BinaryLog,  // .blg read through PDH
  Csv,        // "(PDH-CSV 4.0)" export
  Tsv,        // "(PDH-TSV 4.0)" export
};

struct PerformanceLogTimestamp {
  int year{0};
  int month{0};
  int day{0};
  int hour{0};
  int minute{0};
  int second{0};
  int millisecond{0};
  bool valid{false};
};

struct PerformanceCounterPath {
  std::string machine;
  std::string object;
  std::string instance;
  std::string counter;
  std::string full;
};

struct PerformanceCounterStatistics {
  double minimum{0.0};
  double maximum{0.0};
  double average{0.0};
  double first{0.0};
  double last{0.0};
  std::size_t values{0};
  std::size_t gaps{0};
};

struct PerformanceCounterSeries {
  PerformanceCounterPath path;
  std::vector<double> values;
  PerformanceCounterStatistics statistics;

  [[nodiscard]] bool has_values() const noexcept {
    return statistics.values != 0;
  }
};

struct PerformanceLogDocument {
  PerformanceLogSourceKind source_kind{PerformanceLogSourceKind::Csv};
  std::string machine;
  std::string time_zone;
  // A binary log carries no zone name, only the local time of the machine
  // that recorded it; a text export names its zone in the header instead.
  bool node_local_time{false};
  std::vector<PerformanceLogTimestamp> samples;
  std::vector<PerformanceCounterSeries> counters;
  std::size_t skipped_rows{0};
  unsigned long error{0};
  bool cancelled{false};
  bool truncated{false};
};

enum class PerformanceLogInterpretationKind {
  Information,
  Warning,
  Error,
};

struct PerformanceLogInterpretation {
  PerformanceLogInterpretationKind kind{
      PerformanceLogInterpretationKind::Information};
  std::string summary;
};

enum class PerformanceLogFilter {
  All,
  WithValues,
  Deviations,
  Processor,
  Memory,
  Disk,
  SqlServer,
  Processes,
};

struct PerformanceLogUiState {
  PerformanceLogFilter filter{PerformanceLogFilter::WithValues};
  std::size_t selected_counter{0};
  bool show_samples{false};
};

[[nodiscard]] bool looks_like_performance_log_text(
    std::string_view source) noexcept;
[[nodiscard]] bool looks_like_performance_log_name(
    const std::filesystem::path& path);
[[nodiscard]] PerformanceLogDocument parse_performance_log_text(
    std::string_view source, std::stop_token stop = {});
// Defined in performance_log_binary.cpp: the only part of the model that
// needs PDH, kept behind the same document type as the text parser.
[[nodiscard]] PerformanceLogDocument read_performance_log_binary(
    const std::filesystem::path& path, std::stop_token stop = {});

[[nodiscard]] PerformanceCounterPath parse_performance_counter_path(
    std::string_view value);
// Both readers fill the same series, so both derive their statistics here and
// the projection cannot drift between the binary and the text format.
void finalize_performance_counter_statistics(
    PerformanceCounterSeries& series) noexcept;
[[nodiscard]] std::string performance_log_format_timestamp(
    const PerformanceLogTimestamp& timestamp, bool with_date);
[[nodiscard]] std::string performance_log_format_value(double value,
                                                       bool russian);
// A column of samples from one counter has to keep one fraction width, so the
// reference is the magnitude of the whole series rather than of each value.
[[nodiscard]] std::string performance_log_format_scaled_value(
    double value, double reference, bool russian);
[[nodiscard]] double performance_counter_magnitude(
    const PerformanceCounterStatistics& statistics) noexcept;
[[nodiscard]] std::string performance_log_format_range(
    const PerformanceLogDocument& document, bool russian);
[[nodiscard]] std::uint64_t performance_log_interval_ms(
    const PerformanceLogDocument& document) noexcept;
[[nodiscard]] PerformanceLogInterpretation interpret_performance_counter(
    const PerformanceCounterSeries& series, bool russian);
// Object and counter matching stays language independent: a log recorded on a
// Russian Windows must filter the same way in the English interface.
[[nodiscard]] bool performance_counter_matches_filter(
    const PerformanceCounterSeries& series, PerformanceLogFilter filter);

struct PerformanceChartScale {
  double minimum{0.0};
  double maximum{1.0};
  int decimals{1};
};

struct PerformanceChartPoint {
  int x{0};
  int y{0};

  bool operator==(const PerformanceChartPoint&) const = default;
};

[[nodiscard]] PerformanceChartScale performance_chart_scale(
    const PerformanceCounterStatistics& statistics) noexcept;
// A gap ends the current polyline instead of being bridged, so the chart can
// never draw a line across samples the log does not contain.
[[nodiscard]] std::vector<std::vector<PerformanceChartPoint>>
performance_chart_segments(const LayoutRect& plot,
                           const std::vector<double>& values,
                           const PerformanceChartScale& scale);

struct PerformanceLogLayout {
  LayoutRect summary;
  LayoutRect filter;
  LayoutRect counters;
  LayoutRect samples_toggle;
  LayoutRect chart;
  LayoutRect samples;
};

[[nodiscard]] PerformanceLogLayout calculate_performance_log_layout(
    int width, int height, int dpi, bool show_samples) noexcept;

}  // namespace listopad
