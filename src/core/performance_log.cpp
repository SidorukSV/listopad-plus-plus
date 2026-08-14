#include "listopad/performance_log.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

namespace listopad {
namespace {

// A counter path plus one double per sample is the whole in-memory model, so
// the product of both dimensions is what has to stay bounded. 16 Mi values is
// 128 MiB of samples, well past any hand-collected run and still far from the
// address space of a 64-bit editor.
constexpr std::size_t kMaxCounters = 4096;
constexpr std::size_t kMaxValues = 16u * 1024u * 1024u;

constexpr std::string_view kCsvSignature = "(PDH-CSV 4.0)";
constexpr std::string_view kTsvSignature = "(PDH-TSV 4.0)";

bool ascii_space(const char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

std::string_view trimmed(std::string_view value) noexcept {
  while (!value.empty() && ascii_space(value.front())) value.remove_prefix(1);
  while (!value.empty() && ascii_space(value.back())) value.remove_suffix(1);
  return value;
}

char ascii_lower(const unsigned char value) noexcept {
  return value >= 'A' && value <= 'Z'
             ? static_cast<char>(value - 'A' + 'a')
             : static_cast<char>(value);
}

// Cyrillic patterns are compared byte for byte because counter names arrive
// from Windows in a fixed casing; only the ASCII patterns need folding.
bool contains_ascii_ci(const std::string_view haystack,
                       const std::string_view needle) noexcept {
  if (needle.empty()) return true;
  if (haystack.size() < needle.size()) return false;
  const std::size_t last = haystack.size() - needle.size();
  for (std::size_t start = 0; start <= last; ++start) {
    std::size_t index = 0;
    while (index < needle.size() &&
           ascii_lower(static_cast<unsigned char>(haystack[start + index])) ==
               ascii_lower(static_cast<unsigned char>(needle[index]))) {
      ++index;
    }
    if (index == needle.size()) return true;
  }
  return false;
}

bool contains(const std::string_view haystack,
              const std::string_view needle) noexcept {
  return !needle.empty() &&
         haystack.find(needle) != std::string_view::npos;
}

// Either spelling is accepted regardless of the interface language.
bool matches_either(const std::string_view value,
                    const std::string_view russian,
                    const std::string_view english) noexcept {
  return contains(value, russian) || contains_ascii_ci(value, english);
}

std::string comma_decimal(std::string value, const bool russian) {
  if (!russian) return value;
  const std::size_t dot = value.find('.');
  if (dot != std::string::npos) value[dot] = ',';
  return value;
}

std::optional<double> parse_number(std::string_view token) noexcept {
  token = trimmed(token);
  if (token.empty()) return std::nullopt;
  // PDH writes an invariant decimal point, but an exported log that passed
  // through a localized tool can carry a comma instead.
  std::array<char, 64> buffer{};
  if (token.size() >= buffer.size()) return std::nullopt;
  for (std::size_t index = 0; index < token.size(); ++index) {
    buffer[index] = token[index] == ',' ? '.' : token[index];
  }
  double value = 0.0;
  const char* begin = buffer.data();
  const char* end = begin + token.size();
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
  if (!std::isfinite(value)) return std::nullopt;
  return value;
}

std::optional<int> parse_integer(const std::string_view token) noexcept {
  int value = 0;
  const char* begin = token.data();
  const char* end = begin + token.size();
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
  return value;
}

// PDH timestamps are written as MM/DD/YYYY HH:MM:SS.mmm in both text formats.
PerformanceLogTimestamp parse_timestamp(std::string_view value) noexcept {
  PerformanceLogTimestamp result;
  value = trimmed(value);
  if (value.size() < 19) return result;
  const auto part = [value](const std::size_t offset,
                            const std::size_t length) {
    return value.substr(offset, length);
  };
  if (value[2] != '/' || value[5] != '/' || value[10] != ' ' ||
      value[13] != ':' || value[16] != ':') {
    return result;
  }
  const auto month = parse_integer(part(0, 2));
  const auto day = parse_integer(part(3, 2));
  const auto year = parse_integer(part(6, 4));
  const auto hour = parse_integer(part(11, 2));
  const auto minute = parse_integer(part(14, 2));
  const auto second = parse_integer(part(17, 2));
  if (!month || !day || !year || !hour || !minute || !second) return result;
  int millisecond = 0;
  if (value.size() >= 23 && value[19] == '.') {
    const auto parsed = parse_integer(part(20, 3));
    if (!parsed) return result;
    millisecond = *parsed;
  }
  if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 ||
      *minute > 59 || *second > 60) {
    return result;
  }
  result = {
      .year = *year,
      .month = *month,
      .day = *day,
      .hour = *hour,
      .minute = *minute,
      .second = *second,
      .millisecond = millisecond,
      .valid = true,
  };
  return result;
}

std::int64_t days_from_civil(const int year, const int month,
                             const int day) noexcept {
  int shifted = year - (month <= 2 ? 1 : 0);
  const int era = (shifted >= 0 ? shifted : shifted - 399) / 400;
  const int year_of_era = shifted - era * 400;
  const int day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<std::int64_t>(era) * 146097 +
         static_cast<std::int64_t>(day_of_era) - 719468;
}

std::int64_t milliseconds_since_epoch(
    const PerformanceLogTimestamp& timestamp) noexcept {
  const std::int64_t days =
      days_from_civil(timestamp.year, timestamp.month, timestamp.day);
  return ((days * 24 + timestamp.hour) * 60 + timestamp.minute) * 60000 +
         static_cast<std::int64_t>(timestamp.second) * 1000 +
         timestamp.millisecond;
}

// A threshold is a round configured number, so it is printed without the
// fixed fraction the measured aggregates use.
std::string format_threshold(const double value, const bool russian) {
  std::array<char, 64> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.6g", value);
  return comma_decimal(buffer.data(), russian);
}

enum class Aggregate { Average, Minimum, Maximum };
enum class Comparison { Above, Below };

struct ThresholdRule {
  std::string_view object_russian;
  std::string_view object_english;
  std::string_view counter_russian;
  std::string_view counter_english;
  Aggregate aggregate;
  Comparison comparison;
  double warning;
  double error;
  std::string_view unit_russian;
  std::string_view unit_english;
};

// Thresholds are the conventional Windows capacity-planning marks. They are
// reported as measured aggregate against a named threshold, never as a cause.
constexpr std::array<ThresholdRule, 8> kRules{{
    {"роцессор", "Processor", "загруженности процессора", "% Processor Time",
     Aggregate::Average, Comparison::Above, 90.0, 95.0, "%", "%"},
    {"истема", "System", "Очередь процессора", "Processor Queue Length",
     Aggregate::Average, Comparison::Above, 10.0, 20.0, "", ""},
    {"", "", "Доступно МБ", "Available MBytes", Aggregate::Minimum,
     Comparison::Below, 512.0, 128.0, "МБ", "MB"},
    {"", "", "использования выделенной памяти", "% Committed Bytes In Use",
     Aggregate::Average, Comparison::Above, 90.0, 95.0, "%", "%"},
    {"", "", "Средняя длина очереди диска", "Avg. Disk Queue Length",
     Aggregate::Average, Comparison::Above, 2.0, 8.0, "", ""},
    {"диск", "Disk", "Среднее время", "Avg. Disk sec/", Aggregate::Average,
     Comparison::Above, 0.02, 0.05, "с", "s"},
    {"Файл подкачки", "Paging File", "использования", "% Usage",
     Aggregate::Maximum, Comparison::Above, 80.0, 95.0, "%", "%"},
    {"", "", "Обмен страниц в секунду", "Pages/sec", Aggregate::Average,
     Comparison::Above, 1000.0, 5000.0, "", ""},
}};

const ThresholdRule* find_rule(const PerformanceCounterPath& path) noexcept {
  for (const ThresholdRule& rule : kRules) {
    if (!rule.object_russian.empty() &&
        !matches_either(path.object, rule.object_russian,
                        rule.object_english)) {
      continue;
    }
    if (!matches_either(path.counter, rule.counter_russian,
                        rule.counter_english)) {
      continue;
    }
    return &rule;
  }
  return nullptr;
}

}  // namespace

void finalize_performance_counter_statistics(
    PerformanceCounterSeries& series) noexcept {
  PerformanceCounterStatistics statistics;
  double sum = 0.0;
  bool started = false;
  for (const double value : series.values) {
    if (!performance_log_has_value(value)) {
      ++statistics.gaps;
      continue;
    }
    if (!started) {
      statistics.minimum = value;
      statistics.maximum = value;
      statistics.first = value;
      started = true;
    } else {
      statistics.minimum = (std::min)(statistics.minimum, value);
      statistics.maximum = (std::max)(statistics.maximum, value);
    }
    statistics.last = value;
    sum += value;
    ++statistics.values;
  }
  if (statistics.values != 0) {
    statistics.average = sum / static_cast<double>(statistics.values);
  }
  series.statistics = statistics;
}

namespace {

// PDH quotes every cell and doubles an embedded quotation mark. The reader
// keeps that grammar rather than splitting on the separator alone, so a
// separator inside an instance name cannot shift the whole row.
class RowReader final {
 public:
  RowReader(const std::string_view source, const char separator) noexcept
      : source_(source), separator_(separator) {
    if (source_.size() >= 3 &&
        static_cast<unsigned char>(source_[0]) == 0xef &&
        static_cast<unsigned char>(source_[1]) == 0xbb &&
        static_cast<unsigned char>(source_[2]) == 0xbf) {
      position_ = 3;
    }
  }

  [[nodiscard]] bool done() const noexcept {
    return position_ >= source_.size();
  }

  bool next(std::vector<std::string>& cells) {
    cells.clear();
    if (done()) return false;
    std::string cell;
    bool quoted = false;
    bool any = false;
    while (position_ < source_.size()) {
      const char value = source_[position_];
      if (quoted) {
        if (value == '"') {
          if (position_ + 1 < source_.size() && source_[position_ + 1] == '"') {
            cell.push_back('"');
            position_ += 2;
            continue;
          }
          quoted = false;
          ++position_;
          continue;
        }
        cell.push_back(value);
        ++position_;
        continue;
      }
      if (value == '"') {
        quoted = true;
        any = true;
        ++position_;
        continue;
      }
      if (value == separator_) {
        cells.push_back(std::move(cell));
        cell.clear();
        any = true;
        ++position_;
        continue;
      }
      if (value == '\r' || value == '\n') {
        ++position_;
        if (value == '\r' && position_ < source_.size() &&
            source_[position_] == '\n') {
          ++position_;
        }
        cells.push_back(std::move(cell));
        return true;
      }
      cell.push_back(value);
      any = true;
      ++position_;
    }
    cells.push_back(std::move(cell));
    return any || !cells.front().empty();
  }

 private:
  std::string_view source_;
  char separator_{','};
  std::size_t position_{0};
};

std::string_view header_signature(std::string_view source) noexcept {
  if (source.size() >= 3 &&
      static_cast<unsigned char>(source[0]) == 0xef &&
      static_cast<unsigned char>(source[1]) == 0xbb &&
      static_cast<unsigned char>(source[2]) == 0xbf) {
    source.remove_prefix(3);
  }
  if (!source.empty() && source.front() == '"') source.remove_prefix(1);
  return source;
}

// "(PDH-CSV 4.0) (Russia TZ 4 Standard Time)(-300)" carries the zone the
// logging machine used; it is reported verbatim instead of being converted.
std::string parse_time_zone(const std::string_view header) {
  const std::size_t signature_end = header.find(')');
  if (signature_end == std::string_view::npos) return {};
  std::string result;
  std::size_t position = signature_end + 1;
  while (position < header.size()) {
    const std::size_t open = header.find('(', position);
    if (open == std::string_view::npos) break;
    const std::size_t close = header.find(')', open + 1);
    if (close == std::string_view::npos) break;
    const std::string_view group =
        trimmed(header.substr(open + 1, close - open - 1));
    if (!group.empty()) {
      if (!result.empty()) result += ' ';
      result += group;
    }
    position = close + 1;
  }
  return result;
}

}  // namespace

bool performance_log_has_value(const double value) noexcept {
  return value != kPerformanceLogNoValue && std::isfinite(value);
}

bool looks_like_performance_log_text(const std::string_view source) noexcept {
  const std::string_view header = header_signature(source);
  return header.starts_with(kCsvSignature) ||
         header.starts_with(kTsvSignature);
}

bool looks_like_performance_log_name(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  for (wchar_t& value : extension) {
    if (value >= L'A' && value <= L'Z') value = value - L'A' + L'a';
  }
  return extension == L".blg";
}

PerformanceCounterPath parse_performance_counter_path(
    const std::string_view value) {
  PerformanceCounterPath path;
  path.full = std::string(trimmed(value));
  std::string_view rest = path.full;
  if (rest.starts_with("\\\\")) {
    rest.remove_prefix(2);
    const std::size_t machine_end = rest.find('\\');
    if (machine_end == std::string_view::npos) {
      path.machine = std::string(rest);
      return path;
    }
    path.machine = std::string(rest.substr(0, machine_end));
    rest.remove_prefix(machine_end + 1);
  } else if (rest.starts_with("\\")) {
    rest.remove_prefix(1);
  }
  // The counter name never contains a backslash, so the last separator splits
  // the object (with its optional instance) from the counter.
  const std::size_t counter_begin = rest.rfind('\\');
  if (counter_begin == std::string_view::npos) {
    path.object = std::string(rest);
    return path;
  }
  path.counter = std::string(rest.substr(counter_begin + 1));
  std::string_view object = rest.substr(0, counter_begin);
  const std::size_t instance_begin = object.find('(');
  if (instance_begin != std::string_view::npos && object.back() == ')') {
    path.instance = std::string(object.substr(
        instance_begin + 1, object.size() - instance_begin - 2));
    object = object.substr(0, instance_begin);
  }
  path.object = std::string(object);
  return path;
}

PerformanceLogDocument parse_performance_log_text(
    const std::string_view source, const std::stop_token stop) {
  PerformanceLogDocument document;
  const std::string_view header = header_signature(source);
  if (header.starts_with(kTsvSignature)) {
    document.source_kind = PerformanceLogSourceKind::Tsv;
  } else if (header.starts_with(kCsvSignature)) {
    document.source_kind = PerformanceLogSourceKind::Csv;
  } else {
    return document;
  }

  RowReader reader(
      source,
      document.source_kind == PerformanceLogSourceKind::Tsv ? '\t' : ',');
  std::vector<std::string> cells;
  if (!reader.next(cells) || cells.size() < 2) return document;

  document.time_zone = parse_time_zone(cells.front());
  const std::size_t counters =
      (std::min)(cells.size() - 1, kMaxCounters);
  if (counters + 1 < cells.size()) document.truncated = true;
  document.counters.reserve(counters);
  for (std::size_t index = 0; index < counters; ++index) {
    PerformanceCounterSeries series;
    series.path = parse_performance_counter_path(cells[index + 1]);
    document.counters.push_back(std::move(series));
  }
  if (!document.counters.empty()) {
    document.machine = document.counters.front().path.machine;
  }

  const std::size_t max_samples =
      counters == 0 ? 0 : (std::max)(std::size_t{1}, kMaxValues / counters);
  std::size_t row = 0;
  while (!reader.done()) {
    if ((row & 0xffu) == 0 && stop.stop_requested()) {
      document.cancelled = true;
      return document;
    }
    ++row;
    if (!reader.next(cells)) break;
    if (cells.size() < 2 && trimmed(cells.front()).empty()) continue;
    const PerformanceLogTimestamp timestamp = parse_timestamp(cells.front());
    if (!timestamp.valid) {
      ++document.skipped_rows;
      continue;
    }
    if (document.samples.size() >= max_samples) {
      document.truncated = true;
      break;
    }
    document.samples.push_back(timestamp);
    for (std::size_t index = 0; index < counters; ++index) {
      const std::optional<double> value =
          index + 1 < cells.size() ? parse_number(cells[index + 1])
                                   : std::nullopt;
      document.counters[index].values.push_back(
          value ? *value : kPerformanceLogNoValue);
    }
  }

  for (PerformanceCounterSeries& series : document.counters) {
    if (stop.stop_requested()) {
      document.cancelled = true;
      return document;
    }
    finalize_performance_counter_statistics(series);
  }
  return document;
}

std::string performance_log_format_timestamp(
    const PerformanceLogTimestamp& timestamp, const bool with_date) {
  if (!timestamp.valid) return "—";
  std::array<char, 64> buffer{};
  if (with_date) {
    std::snprintf(buffer.data(), buffer.size(),
                  "%04d-%02d-%02d %02d:%02d:%02d.%03d", timestamp.year,
                  timestamp.month, timestamp.day, timestamp.hour,
                  timestamp.minute, timestamp.second, timestamp.millisecond);
  } else {
    std::snprintf(buffer.data(), buffer.size(), "%02d:%02d:%02d.%03d",
                  timestamp.hour, timestamp.minute, timestamp.second,
                  timestamp.millisecond);
  }
  return buffer.data();
}

std::string performance_log_format_value(const double value,
                                         const bool russian) {
  return performance_log_format_scaled_value(value, value, russian);
}

double performance_counter_magnitude(
    const PerformanceCounterStatistics& statistics) noexcept {
  return (std::max)(std::fabs(statistics.minimum),
                    std::fabs(statistics.maximum));
}

std::string performance_log_format_scaled_value(const double value,
                                                const double reference,
                                                const bool russian) {
  if (!performance_log_has_value(value)) return "—";
  const double magnitude =
      performance_log_has_value(reference) ? std::fabs(reference) : 0.0;
  const char* format = "%.6f";
  if (magnitude >= 1000.0) {
    format = "%.0f";
  } else if (magnitude >= 10.0) {
    format = "%.2f";
  } else if (magnitude >= 0.01 || magnitude == 0.0) {
    format = "%.3f";
  }
  std::array<char, 64> buffer{};
  std::snprintf(buffer.data(), buffer.size(), format, value);
  return comma_decimal(buffer.data(), russian);
}

std::string performance_log_format_range(
    const PerformanceLogDocument& document, const bool russian) {
  if (document.samples.empty()) {
    return russian ? "нет отсчётов" : "no samples";
  }
  std::string result =
      performance_log_format_timestamp(document.samples.front(), true);
  result += " — ";
  const PerformanceLogTimestamp& last = document.samples.back();
  const PerformanceLogTimestamp& first = document.samples.front();
  const bool same_day = first.year == last.year && first.month == last.month &&
                        first.day == last.day;
  result += performance_log_format_timestamp(last, !same_day);
  return result;
}

std::uint64_t performance_log_interval_ms(
    const PerformanceLogDocument& document) noexcept {
  if (document.samples.size() < 2) return 0;
  const PerformanceLogTimestamp& first = document.samples.front();
  const PerformanceLogTimestamp& last = document.samples.back();
  if (!first.valid || !last.valid) return 0;
  const std::int64_t span =
      milliseconds_since_epoch(last) - milliseconds_since_epoch(first);
  if (span <= 0) return 0;
  return static_cast<std::uint64_t>(span) /
         static_cast<std::uint64_t>(document.samples.size() - 1);
}

PerformanceLogInterpretation interpret_performance_counter(
    const PerformanceCounterSeries& series, const bool russian) {
  PerformanceLogInterpretation result;
  if (!series.has_values()) {
    result.summary = russian ? "Нет отсчётов" : "No samples";
    return result;
  }
  const ThresholdRule* rule = find_rule(series.path);
  if (!rule) {
    result.summary = russian ? "Порог не задан" : "No threshold";
    return result;
  }

  double measured = series.statistics.average;
  std::string_view label = russian ? "Среднее" : "Average";
  if (rule->aggregate == Aggregate::Minimum) {
    measured = series.statistics.minimum;
    label = russian ? "Минимум" : "Minimum";
  } else if (rule->aggregate == Aggregate::Maximum) {
    measured = series.statistics.maximum;
    label = russian ? "Максимум" : "Maximum";
  }

  const auto exceeds = [rule](const double value, const double threshold) {
    return rule->comparison == Comparison::Above ? value > threshold
                                                 : value < threshold;
  };
  double threshold = rule->warning;
  if (exceeds(measured, rule->error)) {
    threshold = rule->error;
    result.kind = PerformanceLogInterpretationKind::Error;
  } else if (exceeds(measured, rule->warning)) {
    result.kind = PerformanceLogInterpretationKind::Warning;
  }

  const double reference = performance_counter_magnitude(series.statistics);
  const std::string_view unit =
      russian ? rule->unit_russian : rule->unit_english;
  const auto with_unit = [unit](std::string value) {
    if (!unit.empty()) {
      value += ' ';
      value += unit;
    }
    return value;
  };

  result.summary = label;
  result.summary += ' ';
  result.summary += with_unit(
      performance_log_format_scaled_value(measured, reference, russian));
  result.summary += " — ";
  if (result.kind == PerformanceLogInterpretationKind::Information) {
    result.summary += russian ? "в пределах порога " : "within the ";
  } else {
    result.summary += rule->comparison == Comparison::Above
                          ? (russian ? "выше порога " : "above the ")
                          : (russian ? "ниже порога " : "below the ");
  }
  result.summary += with_unit(format_threshold(threshold, russian));
  if (!russian) result.summary += " threshold";
  return result;
}

bool performance_counter_matches_filter(
    const PerformanceCounterSeries& series,
    const PerformanceLogFilter filter) {
  switch (filter) {
    case PerformanceLogFilter::All:
      return true;
    case PerformanceLogFilter::WithValues:
      return series.has_values();
    case PerformanceLogFilter::Deviations:
      return interpret_performance_counter(series, false).kind !=
             PerformanceLogInterpretationKind::Information;
    case PerformanceLogFilter::Processor:
      return matches_either(series.path.object, "роцессор", "Processor") ||
             matches_either(series.path.counter, "загруженности процессора",
                            "% Processor Time");
    case PerformanceLogFilter::Memory:
      return matches_either(series.path.object, "амять", "Memory") ||
             matches_either(series.path.object, "Файл подкачки",
                            "Paging File") ||
             matches_either(series.path.counter, "Рабочее множество",
                            "Working Set");
    case PerformanceLogFilter::Disk:
      return matches_either(series.path.object, "диск", "Disk");
  }
  return true;
}

PerformanceChartScale performance_chart_scale(
    const PerformanceCounterStatistics& statistics) noexcept {
  PerformanceChartScale scale;
  if (statistics.values == 0) return scale;
  double minimum = statistics.minimum;
  double maximum = statistics.maximum;
  if (!(maximum > minimum)) {
    const double pad = std::fabs(maximum) > 0.0 ? std::fabs(maximum) / 8.0 : 1.0;
    minimum = maximum - pad;
    maximum = maximum + pad;
  }
  // A non-negative counter is easier to compare across screenshots when the
  // baseline stays at zero, but only while zero does not flatten the shape.
  if (statistics.minimum >= 0.0 &&
      statistics.minimum <= maximum - statistics.minimum) {
    minimum = 0.0;
  }

  const double span = maximum - minimum;
  const double magnitude =
      std::pow(10.0, std::floor(std::log10(span > 0.0 ? span : 1.0)));
  double step = magnitude;
  for (const double factor : {1.0, 2.0, 5.0, 10.0}) {
    step = magnitude * factor;
    if (span / step <= 5.0) break;
  }
  if (!(step > 0.0)) step = 1.0;
  scale.minimum = std::floor(minimum / step) * step;
  scale.maximum = std::ceil(maximum / step) * step;
  if (!(scale.maximum > scale.minimum)) scale.maximum = scale.minimum + step;
  scale.decimals = step >= 1.0     ? 0
                   : step >= 0.1   ? 1
                   : step >= 0.01  ? 2
                   : step >= 0.001 ? 3
                                   : 6;
  return scale;
}

std::vector<std::vector<PerformanceChartPoint>> performance_chart_segments(
    const LayoutRect& plot, const std::vector<double>& values,
    const PerformanceChartScale& scale) {
  std::vector<std::vector<PerformanceChartPoint>> segments;
  if (values.empty() || plot.width <= 0 || plot.height <= 0) return segments;
  const double span = scale.maximum - scale.minimum;
  const int last_x = plot.width - 1;
  const int last_y = plot.height - 1;
  const double divisor =
      values.size() > 1 ? static_cast<double>(values.size() - 1) : 1.0;

  std::vector<PerformanceChartPoint> current;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (!performance_log_has_value(values[index])) {
      if (!current.empty()) segments.push_back(std::move(current));
      current.clear();
      continue;
    }
    const double ratio =
        values.size() > 1 ? static_cast<double>(index) / divisor : 0.5;
    const double normalized =
        span > 0.0 ? (values[index] - scale.minimum) / span : 0.5;
    const double clamped = (std::min)(1.0, (std::max)(0.0, normalized));
    current.push_back({
        .x = plot.x + static_cast<int>(std::lround(ratio * last_x)),
        .y = plot.y + last_y -
             static_cast<int>(std::lround(clamped * last_y)),
    });
  }
  if (!current.empty()) segments.push_back(std::move(current));
  return segments;
}

PerformanceLogLayout calculate_performance_log_layout(
    const int width, const int height, const int dpi,
    const bool show_samples) noexcept {
  const int safe_width = (std::max)(0, width);
  const int safe_height = (std::max)(0, height);
  const int margin = (std::max)(4, dpi * 6 / 96);
  const int row = (std::max)(24, dpi * 28 / 96);
  const int toggle = (std::max)(22, dpi * 24 / 96);
  const int top = margin;
  const int summary_width =
      (std::max)(0, safe_width - margin * 3 - dpi * 200 / 96);
  const int filter_width =
      (std::max)(0, safe_width - margin * 3 - summary_width);
  const int table_top = top + row + margin;
  const int content_bottom = (std::max)(table_top, safe_height - margin);
  const int available = content_bottom - table_top;
  const int table_height = available * 40 / 100;
  const int toggle_top = table_top + table_height + margin;
  const int chart_top = toggle_top + toggle;
  const int chart_available = (std::max)(0, content_bottom - chart_top);
  // The chart keeps the larger share: the sample table is the verification
  // panel, and a plot squeezed below a few rows of pixels shows nothing.
  const int samples_height = show_samples ? chart_available * 40 / 100 : 0;

  return {
      .summary = {margin, top, summary_width, row},
      .filter = {margin * 2 + summary_width, top, filter_width, row},
      .counters = {margin, table_top, safe_width - margin * 2, table_height},
      .samples_toggle = {margin, toggle_top, safe_width - margin * 2, toggle},
      .chart = {margin, chart_top, safe_width - margin * 2,
                chart_available - samples_height -
                    (show_samples ? margin : 0)},
      .samples = {margin, chart_top + chart_available - samples_height,
                  safe_width - margin * 2, samples_height},
  };
}

}  // namespace listopad
