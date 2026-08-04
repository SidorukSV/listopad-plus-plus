#include "listopad/technology_log.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <limits>
#include <optional>

namespace listopad {
namespace {

struct ParsedPrefix {
  int minute{0};
  int second{0};
  int microsecond{0};
  std::uint64_t duration_us{0};
  std::size_t type_begin{0};
  std::size_t type_end{0};
  int nesting_level{0};
  std::size_t fields_begin{0};
};

bool ascii_space(const char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

std::pair<std::size_t, std::size_t> trimmed_range(
    const std::string_view source, std::size_t begin,
    std::size_t end) noexcept {
  while (begin < end && ascii_space(source[begin])) ++begin;
  while (end > begin && ascii_space(source[end - 1])) --end;
  return {begin, end};
}

bool parse_decimal(const std::string_view source, std::size_t& position,
                   const std::size_t minimum_digits,
                   const std::size_t maximum_digits,
                   std::uint64_t& value) noexcept {
  const std::size_t begin = position;
  value = 0;
  while (position < source.size() &&
         position - begin < maximum_digits &&
         source[position] >= '0' && source[position] <= '9') {
    const unsigned digit = static_cast<unsigned>(source[position] - '0');
    if (value > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
    ++position;
  }
  return position - begin >= minimum_digits;
}

bool take(const std::string_view source, std::size_t& position,
          const char expected) noexcept {
  if (position >= source.size() || source[position] != expected) return false;
  ++position;
  return true;
}

std::optional<ParsedPrefix> parse_prefix(const std::string_view source,
                                         const std::size_t start) noexcept {
  std::size_t position = start;
  std::uint64_t minute = 0;
  std::uint64_t second = 0;
  std::uint64_t microsecond = 0;
  std::uint64_t duration = 0;
  std::uint64_t nesting = 0;
  if (!parse_decimal(source, position, 1, 2, minute) ||
      minute > 59 || !take(source, position, ':') ||
      !parse_decimal(source, position, 2, 2, second) ||
      second > 59 || !take(source, position, '.') ||
      !parse_decimal(source, position, 6, 6, microsecond) ||
      !take(source, position, '-') ||
      !parse_decimal(source, position, 1, 20, duration) ||
      !take(source, position, ',')) {
    return std::nullopt;
  }

  const std::size_t type_begin = position;
  while (position < source.size() && source[position] != ',' &&
         source[position] != '\r' && source[position] != '\n') {
    ++position;
  }
  if (position == type_begin || !take(source, position, ',')) {
    return std::nullopt;
  }
  const std::size_t type_end = position - 1;
  if (!parse_decimal(source, position, 1, 10, nesting) ||
      nesting > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()) ||
      !take(source, position, ',')) {
    return std::nullopt;
  }

  return ParsedPrefix{
      .minute = static_cast<int>(minute),
      .second = static_cast<int>(second),
      .microsecond = static_cast<int>(microsecond),
      .duration_us = duration,
      .type_begin = type_begin,
      .type_end = type_end,
      .nesting_level = static_cast<int>(nesting),
      .fields_begin = position,
  };
}

bool record_start(const std::string_view source,
                  const std::size_t position) noexcept {
  if (position >= source.size()) return false;
  const bool after_bom =
      position == 3 && source.size() >= 3 &&
      static_cast<unsigned char>(source[0]) == 0xef &&
      static_cast<unsigned char>(source[1]) == 0xbb &&
      static_cast<unsigned char>(source[2]) == 0xbf;
  if (position != 0 && !after_bom && source[position - 1] != '\n') {
    return false;
  }
  return parse_prefix(source, position).has_value();
}

bool ascii_iequal(const std::string_view left,
                  const std::string_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto lower = [](const unsigned char value) {
      return value >= 'A' && value <= 'Z'
                 ? static_cast<unsigned char>(value - 'A' + 'a')
                 : value;
    };
    if (lower(static_cast<unsigned char>(left[index])) !=
        lower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

std::optional<TechnologyLogFileTime> parse_file_time(
    std::string_view file_name) noexcept {
  const std::size_t slash = file_name.find_last_of("/\\");
  if (slash != std::string_view::npos) file_name.remove_prefix(slash + 1);
  if (file_name.size() < 8) return std::nullopt;
  for (std::size_t index = 0; index < 8; ++index) {
    if (file_name[index] < '0' || file_name[index] > '9') return std::nullopt;
  }
  const auto two = [file_name](const std::size_t index) {
    return (file_name[index] - '0') * 10 + (file_name[index + 1] - '0');
  };
  TechnologyLogFileTime time{
      .year = 2000 + two(0),
      .month = two(2),
      .day = two(4),
      .hour = two(6),
      .valid = true,
  };
  if (time.month < 1 || time.month > 12 || time.day < 1 || time.day > 31 ||
      time.hour < 0 || time.hour > 23) {
    return std::nullopt;
  }
  return time;
}

bool parse_fields(const std::string_view source,
                  const std::size_t begin, const std::size_t end,
                  TechnologyLogEvent& event,
                  const std::stop_token stop) {
  std::size_t token_begin = begin;
  std::size_t position = begin;
  char quote = '\0';
  const auto append = [&](const std::size_t token_end) {
    auto [trimmed_begin, trimmed_end] =
        trimmed_range(source, token_begin, token_end);
    if (trimmed_begin == trimmed_end) return;
    std::size_t equals = trimmed_begin;
    char nested_quote = '\0';
    for (; equals < trimmed_end; ++equals) {
      if (((equals - trimmed_begin) & 0xffffu) == 0 &&
          stop.stop_requested()) {
        return;
      }
      const char value = source[equals];
      if (nested_quote != '\0') {
        if (value == nested_quote && equals + 1 < trimmed_end &&
            source[equals + 1] == nested_quote) {
          ++equals;
        } else if (value == nested_quote) {
          nested_quote = '\0';
        }
      } else if (value == '\'' || value == '"') {
        nested_quote = value;
      } else if (value == '=') {
        break;
      }
    }
    if (equals >= trimmed_end) return;
    auto [name_begin, name_end] =
        trimmed_range(source, trimmed_begin, equals);
    auto [value_begin, value_end] =
        trimmed_range(source, equals + 1, trimmed_end);
    if (name_begin == name_end) return;
    char value_quote = '\0';
    if (value_begin < value_end &&
        (source[value_begin] == '\'' || source[value_begin] == '"')) {
      value_quote = source[value_begin];
      ++value_begin;
      if (value_end > value_begin && source[value_end - 1] == value_quote) {
        --value_end;
      } else {
        event.incomplete = true;
      }
    }
    event.fields.push_back({
        .name = {.offset = name_begin, .length = name_end - name_begin},
        .value = {.offset = value_begin,
                  .length = value_end - value_begin},
        .quote = value_quote,
    });
  };

  while (position < end) {
    if (((position - begin) & 0xffffu) == 0 &&
        stop.stop_requested()) {
      return false;
    }
    const char value = source[position];
    if (quote != '\0') {
      if (value == quote && position + 1 < end &&
          source[position + 1] == quote) {
        position += 2;
        continue;
      }
      if (value == quote) quote = '\0';
    } else if (value == '\'' || value == '"') {
      quote = value;
    } else if (value == ',') {
      append(position);
      token_begin = position + 1;
    }
    ++position;
  }
  append(end);
  if (stop.stop_requested()) return false;
  if (quote != '\0') event.incomplete = true;
  return true;
}

std::string unescape_quoted(const std::string_view value,
                            const char quote) {
  if (quote == '\0' || value.find(std::string(2, quote)) ==
                             std::string_view::npos) {
    return std::string(value);
  }
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    result.push_back(value[index]);
    if (value[index] == quote && index + 1 < value.size() &&
        value[index + 1] == quote) {
      ++index;
    }
  }
  return result;
}

std::string first_line(std::string value) {
  const std::size_t end = value.find_first_of("\r\n");
  if (end != std::string::npos) value.resize(end);
  auto [begin, trimmed_end] = trimmed_range(value, 0, value.size());
  value = value.substr(begin, trimmed_end - begin);
  if (value.size() > 240) {
    value.resize(237);
    value += "...";
  }
  return value;
}

bool looks_like_source_location(const std::string_view line) noexcept {
  return line.find(".cpp(") != std::string_view::npos ||
         line.find(".c(") != std::string_view::npos;
}

std::string exception_message(std::string value) {
  std::size_t begin = 0;
  while (begin <= value.size()) {
    const std::size_t separator = value.find_first_of("\r\n", begin);
    const std::size_t end =
        separator == std::string::npos ? value.size() : separator;
    auto [line_begin, line_end] = trimmed_range(value, begin, end);
    std::string line = value.substr(line_begin, line_end - line_begin);
    if (!line.empty() && !looks_like_source_location(line)) {
      if (line.size() > 38 && line[36] == ':' && line[8] == '-' &&
          line[13] == '-' && line[18] == '-' && line[23] == '-') {
        line.erase(0, 37);
        auto [message_begin, message_end] =
            trimmed_range(line, 0, line.size());
        line = line.substr(message_begin, message_end - message_begin);
      }
      return first_line(std::move(line));
    }
    if (separator == std::string::npos) break;
    begin = separator + 1;
    if (begin < value.size() && value[separator] == '\r' &&
        value[begin] == '\n') {
      ++begin;
    }
  }
  return first_line(std::move(value));
}

std::string join_nonempty(std::string left, const std::string_view separator,
                          const std::string& right) {
  if (right.empty()) return left;
  if (!left.empty()) left += separator;
  left += right;
  return left;
}

bool warning_severity(const std::string_view severity) noexcept {
  return ascii_iequal(severity, "WARNING") ||
         ascii_iequal(severity, "ERROR");
}

}  // namespace

bool looks_like_technology_log(const std::string_view source) noexcept {
  std::size_t position = 0;
  if (source.size() >= 3 &&
      static_cast<unsigned char>(source[0]) == 0xef &&
      static_cast<unsigned char>(source[1]) == 0xbb &&
      static_cast<unsigned char>(source[2]) == 0xbf) {
    position = 3;
  }
  for (int line = 0; line < 8 && position < source.size(); ++line) {
    if (const auto prefix = parse_prefix(source, position)) {
      const std::size_t line_end = source.find('\n', prefix->fields_begin);
      const std::string_view header = source.substr(
          prefix->fields_begin,
          (line_end == std::string_view::npos ? source.size() : line_end) -
              prefix->fields_begin);
      return header.find("level=") != std::string_view::npos &&
             (header.find("process=") != std::string_view::npos ||
              header.find("OSThread=") != std::string_view::npos);
    }
    const std::size_t newline = source.find('\n', position);
    if (newline == std::string_view::npos) break;
    position = newline + 1;
  }
  return false;
}

TechnologyLogDocument parse_technology_log(
    const std::string_view source, const std::string_view file_name,
    const std::stop_token stop) {
  TechnologyLogDocument document;
  if (const auto time = parse_file_time(file_name)) {
    document.file_time = *time;
  }

  std::size_t position = 0;
  if (source.size() >= 3 &&
      static_cast<unsigned char>(source[0]) == 0xef &&
      static_cast<unsigned char>(source[1]) == 0xbb &&
      static_cast<unsigned char>(source[2]) == 0xbf) {
    position = 3;
  }
  while (position < source.size() && !record_start(source, position)) {
    const std::size_t newline = source.find('\n', position);
    if (newline == std::string_view::npos) {
      document.skipped_bytes += source.size() - position;
      return document;
    }
    document.skipped_bytes += newline + 1 - position;
    position = newline + 1;
  }

  while (position < source.size()) {
    if (stop.stop_requested()) {
      document.cancelled = true;
      break;
    }
    const auto prefix = parse_prefix(source, position);
    if (!prefix) {
      const std::size_t newline = source.find('\n', position);
      if (newline == std::string_view::npos) {
        document.skipped_bytes += source.size() - position;
        break;
      }
      document.skipped_bytes += newline + 1 - position;
      position = newline + 1;
      continue;
    }

    std::size_t end = source.size();
    std::size_t scan = prefix->fields_begin;
    char quote = '\0';
    while (scan < source.size()) {
      if (((scan - prefix->fields_begin) & 0xffffu) == 0 &&
          stop.stop_requested()) {
        document.cancelled = true;
        return document;
      }
      const char value = source[scan];
      if (quote != '\0') {
        if (value == quote && scan + 1 < source.size() &&
            source[scan + 1] == quote) {
          scan += 2;
          continue;
        }
        if (value == quote) quote = '\0';
      } else if (value == '\'' || value == '"') {
        quote = value;
      } else if (value == '\n') {
        const std::size_t next = scan + 1;
        if (record_start(source, next)) {
          end = scan;
          if (end > position && source[end - 1] == '\r') --end;
          break;
        }
      }
      ++scan;
    }
    if (end == source.size()) {
      while (end > position &&
             (source[end - 1] == '\r' || source[end - 1] == '\n')) {
        --end;
      }
    }

    TechnologyLogEvent event{
        .raw = {.offset = position, .length = end - position},
        .minute = prefix->minute,
        .second = prefix->second,
        .microsecond = prefix->microsecond,
        .duration_us = prefix->duration_us,
        .type = std::string(source.substr(
            prefix->type_begin, prefix->type_end - prefix->type_begin)),
        .nesting_level = prefix->nesting_level,
        .incomplete = quote != '\0',
    };
    if (!parse_fields(source, prefix->fields_begin, end, event, stop)) {
      document.cancelled = true;
      break;
    }
    document.events.push_back(std::move(event));
    if (scan >= source.size()) break;
    position = scan + 1;
  }
  return document;
}

std::string_view technology_log_raw_event(
    const std::string_view source,
    const TechnologyLogEvent& event) noexcept {
  if (event.raw.offset > source.size() ||
      event.raw.length > source.size() - event.raw.offset) {
    return {};
  }
  return source.substr(event.raw.offset, event.raw.length);
}

std::string_view technology_log_field_name(
    const std::string_view source,
    const TechnologyLogField& field) noexcept {
  if (field.name.offset > source.size() ||
      field.name.length > source.size() - field.name.offset) {
    return {};
  }
  return source.substr(field.name.offset, field.name.length);
}

std::string technology_log_field_value(
    const std::string_view source, const TechnologyLogEvent& event,
    const std::string_view name) {
  for (const TechnologyLogField& field : event.fields) {
    if (!ascii_iequal(technology_log_field_name(source, field), name) ||
        field.value.offset > source.size() ||
        field.value.length > source.size() - field.value.offset) {
      continue;
    }
    return unescape_quoted(
        source.substr(field.value.offset, field.value.length), field.quote);
  }
  return {};
}

std::string technology_log_format_timestamp(
    const TechnologyLogDocument& document,
    const TechnologyLogEvent& event) {
  std::array<char, 64> buffer{};
  if (document.file_time.valid) {
    std::snprintf(buffer.data(), buffer.size(),
                  "%04d-%02d-%02d %02d:%02d:%02d.%06d",
                  document.file_time.year, document.file_time.month,
                  document.file_time.day, document.file_time.hour,
                  event.minute, event.second, event.microsecond);
  } else {
    std::snprintf(buffer.data(), buffer.size(), "%02d:%02d.%06d",
                  event.minute, event.second, event.microsecond);
  }
  return buffer.data();
}

std::string technology_log_format_duration(
    const std::uint64_t duration_us, const bool russian) {
  std::array<char, 64> buffer{};
  if (duration_us >= 1'000'000) {
    std::snprintf(buffer.data(), buffer.size(), "%.3f %s",
                  static_cast<double>(duration_us) / 1'000'000.0,
                  russian ? "с" : "s");
  } else if (duration_us >= 1'000) {
    std::snprintf(buffer.data(), buffer.size(), "%.3f %s",
                  static_cast<double>(duration_us) / 1'000.0,
                  russian ? "мс" : "ms");
  } else {
    std::snprintf(buffer.data(), buffer.size(), "%llu %s",
                  static_cast<unsigned long long>(duration_us),
                  russian ? "мкс" : "us");
  }
  std::string result = buffer.data();
  if (russian) {
    const std::size_t dot = result.find('.');
    if (dot != std::string::npos) result[dot] = ',';
  }
  return result;
}

TechnologyLogInterpretation interpret_technology_log_event(
    const std::string_view source, const TechnologyLogEvent& event,
    const bool russian) {
  const std::string severity =
      technology_log_field_value(source, event, "level");
  TechnologyLogInterpretation result;
  if (warning_severity(severity)) {
    result.kind = TechnologyLogInterpretationKind::Warning;
  }

  if (ascii_iequal(event.type, "VRSREQUEST")) {
    const std::string method =
        technology_log_field_value(source, event, "Method");
    const std::string uri =
        technology_log_field_value(source, event, "URI");
    result.summary = russian ? "HTTP-запрос" : "HTTP request";
    result.summary = join_nonempty(std::move(result.summary), " ", method);
    result.summary = join_nonempty(std::move(result.summary), " ", uri);
  } else if (ascii_iequal(event.type, "VRSRESPONSE")) {
    const std::string status =
        technology_log_field_value(source, event, "Status");
    const std::string phrase =
        technology_log_field_value(source, event, "Phrase");
    int status_code = 0;
    const auto parsed_status = std::from_chars(
        status.data(), status.data() + status.size(), status_code);
    if (parsed_status.ec == std::errc{} &&
        parsed_status.ptr == status.data() + status.size()) {
      if (status_code >= 500) {
        result.kind = TechnologyLogInterpretationKind::Error;
      } else if (status_code >= 400) {
        result.kind = TechnologyLogInterpretationKind::Warning;
      }
    }
    result.summary = russian ? "HTTP-ответ" : "HTTP response";
    result.summary = join_nonempty(std::move(result.summary), " ", status);
    result.summary = join_nonempty(std::move(result.summary), " ", phrase);
  } else if (ascii_iequal(event.type, "VRSCACHE")) {
    const std::string action =
        technology_log_field_value(source, event, "action");
    const std::string response =
        technology_log_field_value(source, event, "Result");
    std::string operation = action;
    if (operation.empty()) {
      const std::string sql = first_line(
          technology_log_field_value(source, event, "Sql"));
      const std::size_t separator = sql.find_first_of(" \t");
      operation = sql.substr(0, separator);
    }
    result.summary = russian ? "Запрос к кэшу VRS" : "VRS cache query";
    result.summary =
        join_nonempty(std::move(result.summary), ": ", operation);
    result.summary = join_nonempty(std::move(result.summary), " · ", response);
  } else if (ascii_iequal(event.type, "SCALL")) {
    const std::string interface_name =
        technology_log_field_value(source, event, "IName");
    const std::string method =
        technology_log_field_value(source, event, "MName");
    result.summary = russian ? "Серверный вызов" : "Server call";
    std::string target = interface_name;
    if (!method.empty()) {
      if (!target.empty()) target += '.';
      target += method;
    }
    result.summary = join_nonempty(std::move(result.summary), " ", target);
    if (event.duration_us > 0) {
      result.summary += " · ";
      result.summary +=
          technology_log_format_duration(event.duration_us, russian);
    }
  } else if (ascii_iequal(event.type, "EXCP")) {
    result.kind = TechnologyLogInterpretationKind::Error;
    const std::string description = exception_message(
        technology_log_field_value(source, event, "Descr"));
    result.summary = russian ? "Исключение" : "Exception";
    result.summary =
        join_nonempty(std::move(result.summary), ": ", description);
  } else if (ascii_iequal(event.type, "CONN")) {
    result.summary = russian ? "Соединение" : "Connection";
    result.summary = join_nonempty(
        std::move(result.summary), ": ",
        first_line(technology_log_field_value(source, event, "Txt")));
  } else if (ascii_iequal(event.type, "LIC")) {
    result.summary = russian ? "Лицензирование" : "Licensing";
    const std::string function =
        technology_log_field_value(source, event, "Func");
    const std::string status =
        technology_log_field_value(source, event, "res");
    if (ascii_iequal(status, "error") ||
        ascii_iequal(status, "failed")) {
      result.kind = TechnologyLogInterpretationKind::Warning;
    }
    result.summary =
        join_nonempty(std::move(result.summary), ": ", function);
    result.summary =
        join_nonempty(std::move(result.summary), " · ", status);
  } else if (ascii_iequal(event.type, "SYSTEM")) {
    result.summary = russian ? "Системная операция" : "System operation";
    result.summary = join_nonempty(
        std::move(result.summary), ": ",
        technology_log_field_value(source, event, "operation"));
  } else {
    result.summary = event.type;
    const std::string text = first_line(
        technology_log_field_value(source, event, "Txt"));
    result.summary = join_nonempty(std::move(result.summary), " · ", text);
  }

  if (event.incomplete) {
    result.kind = TechnologyLogInterpretationKind::Warning;
    result.summary += russian ? " · запись оборвана"
                              : " · incomplete record";
  }
  return result;
}

TechnologyLogLayout calculate_technology_log_layout(
    const int width, const int height, const int dpi,
    const bool show_raw) noexcept {
  const int safe_width = (std::max)(0, width);
  const int safe_height = (std::max)(0, height);
  const int margin = (std::max)(4, dpi * 6 / 96);
  const int row = (std::max)(24, dpi * 28 / 96);
  const int toggle = (std::max)(22, dpi * 24 / 96);
  const int top = margin;
  const int summary_width = (std::max)(0, safe_width - margin * 3 -
                                             dpi * 180 / 96);
  const int filter_width = (std::max)(0, safe_width - margin * 3 -
                                            summary_width);
  const int table_top = top + row + margin;
  const int content_bottom = (std::max)(table_top, safe_height - margin);
  const int available = content_bottom - table_top;
  const int table_height = available * 56 / 100;
  const int toggle_top = table_top + table_height + margin;
  const int detail_top = toggle_top + toggle;
  const int detail_available =
      (std::max)(0, content_bottom - detail_top);
  const int raw_height = show_raw ? detail_available / 2 : 0;

  return {
      .summary = {margin, top, summary_width, row},
      .filter = {margin * 2 + summary_width, top, filter_width, row},
      .events = {margin, table_top, safe_width - margin * 2, table_height},
      .raw_toggle = {margin, toggle_top, safe_width - margin * 2, toggle},
      .details = {margin, detail_top, safe_width - margin * 2,
                  detail_available - raw_height -
                      (show_raw ? margin : 0)},
      .raw = {margin,
              detail_top + detail_available - raw_height,
              safe_width - margin * 2, raw_height},
  };
}

}  // namespace listopad
