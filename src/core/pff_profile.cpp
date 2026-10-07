#include "listopad/pff_profile.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <unordered_set>

namespace listopad {
namespace {

constexpr std::string_view kRecordAnchor =
    "AAAAAAAAAAAAAAAAAAAAAAAAAAA=,";

void skip_space(const std::string_view source, std::size_t& position) {
  while (position < source.size() &&
         (source[position] == ' ' || source[position] == '\t' ||
          source[position] == '\r' || source[position] == '\n')) {
    ++position;
  }
}

bool consume(const std::string_view source, std::size_t& position,
             const char expected) {
  skip_space(source, position);
  if (position >= source.size() || source[position] != expected) return false;
  ++position;
  return true;
}

bool parse_string(const std::string_view source, std::size_t& position,
                  std::string& value) {
  skip_space(source, position);
  if (position >= source.size() || source[position] != '"') return false;
  ++position;
  value.clear();
  while (position < source.size()) {
    const char current = source[position++];
    if (current != '"') {
      value.push_back(current);
      continue;
    }
    if (position < source.size() && source[position] == '"') {
      value.push_back('"');
      ++position;
      continue;
    }
    return true;
  }
  return false;
}

template <typename Number>
bool parse_integer(const std::string_view source, std::size_t& position,
                   Number& value) {
  skip_space(source, position);
  const char* first = source.data() + position;
  const char* last = source.data() + source.size();
  const auto parsed = std::from_chars(first, last, value);
  if (parsed.ec != std::errc{} || parsed.ptr == first) return false;
  position = static_cast<std::size_t>(parsed.ptr - source.data());
  return true;
}

bool parse_real(const std::string_view source, std::size_t& position,
                double& value) {
  skip_space(source, position);
  const char* first = source.data() + position;
  const char* last = source.data() + source.size();
  const auto parsed =
      std::from_chars(first, last, value, std::chars_format::general);
  if (parsed.ec != std::errc{} || parsed.ptr == first ||
      !std::isfinite(value)) {
    return false;
  }
  position = static_cast<std::size_t>(parsed.ptr - source.data());
  return true;
}

template <typename Value, typename Parser>
bool comma_value(const std::string_view source, std::size_t& position,
                 Value& value, Parser parser) {
  return consume(source, position, ',') &&
         parser(source, position, value);
}

bool parse_record(const std::string_view source, const std::size_t anchor,
                  PffProfileRecord& record) {
  std::size_t position = anchor + kRecordAnchor.size();
  if (!parse_string(source, position, record.extension) ||
      !consume(source, position, '}') || !consume(source, position, ',') ||
      !parse_string(source, position, record.module) ||
      !comma_value(source, position, record.line,
                   parse_integer<std::uint32_t>) ||
      !comma_value(source, position, record.code, parse_string) ||
      !comma_value(source, position, record.calls,
                   parse_integer<std::uint64_t>) ||
      !comma_value(source, position, record.inclusive_time, parse_real) ||
      !comma_value(source, position, record.self_time, parse_real) ||
      !comma_value(source, position, record.inclusive_share, parse_real) ||
      !comma_value(source, position, record.self_share, parse_real)) {
    return false;
  }

  std::uint32_t client = 0;
  std::uint32_t server = 0;
  std::uint32_t server_call = 0;
  if (!comma_value(source, position, client, parse_integer<std::uint32_t>) ||
      !comma_value(source, position, server, parse_integer<std::uint32_t>) ||
      !comma_value(source, position, server_call,
                   parse_integer<std::uint32_t>)) {
    record.incomplete = true;
    return true;
  }
  record.client = client != 0;
  record.server = server != 0;
  record.server_call = server_call != 0;
  return true;
}

std::string localize_decimal(std::string value, const bool russian) {
  if (russian) {
    const std::size_t dot = value.find('.');
    if (dot != std::string::npos) value[dot] = ',';
  }
  return value;
}

}  // namespace

bool looks_like_pff_profile(const std::string_view source) noexcept {
  const std::size_t anchor = source.find(kRecordAnchor);
  if (anchor == std::string_view::npos || anchor > 64 * 1024) return false;
  PffProfileRecord record;
  return parse_record(source, anchor, record) && !record.module.empty() &&
         record.line != 0;
}

PffProfileDocument parse_pff_profile(const std::string_view source,
                                     const std::stop_token stop) {
  PffProfileDocument document;
  std::unordered_set<std::string> modules;
  std::size_t position = 0;
  while (position < source.size()) {
    if (stop.stop_requested()) {
      document.cancelled = true;
      break;
    }
    const std::size_t anchor = source.find(kRecordAnchor, position);
    if (anchor == std::string_view::npos) break;
    PffProfileRecord record;
    if (parse_record(source, anchor, record)) {
      document.total_self_time += record.self_time;
      modules.insert(record.module);
      document.records.push_back(std::move(record));
    } else {
      ++document.skipped_records;
    }
    position = anchor + kRecordAnchor.size();
  }
  document.module_count = modules.size();
  return document;
}

bool pff_profile_record_matches_filter(
    const PffProfileRecord& record, const PffProfileFilter filter) noexcept {
  switch (filter) {
    case PffProfileFilter::All:
      return true;
    case PffProfileFilter::Hot:
      return record.self_share >= 1.0;
    case PffProfileFilter::Client:
      return record.client;
    case PffProfileFilter::Server:
      return record.server;
    case PffProfileFilter::ServerCalls:
      return record.server_call;
  }
  return true;
}

std::string pff_profile_format_time(const double seconds,
                                    const bool russian) {
  std::array<char, 64> buffer{};
  const double absolute = std::abs(seconds);
  if (absolute < 0.001) {
    std::snprintf(buffer.data(), buffer.size(), "%.3f %s",
                  seconds * 1'000'000.0, russian ? "мкс" : "us");
  } else if (absolute < 1.0) {
    std::snprintf(buffer.data(), buffer.size(), "%.3f %s", seconds * 1000.0,
                  russian ? "мс" : "ms");
  } else {
    std::snprintf(buffer.data(), buffer.size(), "%.3f %s", seconds,
                  russian ? "с" : "s");
  }
  return localize_decimal(buffer.data(), russian);
}

std::string pff_profile_format_share(const double share,
                                     const bool russian) {
  std::array<char, 48> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.3f %%", share);
  return localize_decimal(buffer.data(), russian);
}

std::string pff_profile_context(const PffProfileRecord& record,
                                const bool russian) {
  std::string result;
  const auto append = [&result](const std::string_view value) {
    if (!result.empty()) result += " · ";
    result += value;
  };
  if (record.client) append(russian ? "Клиент" : "Client");
  if (record.server) append(russian ? "Сервер" : "Server");
  if (record.server_call)
    append(russian ? "Вызов сервера" : "Server call");
  return result.empty() ? "—" : result;
}

PffProfileLayout calculate_pff_profile_layout(const int width,
                                              const int height,
                                              const int dpi) noexcept {
  const int unit = (std::max)(1, dpi);
  const auto scale = [unit](const int value) {
    return (std::max)(1, value * unit / 96);
  };
  const int padding = scale(8);
  const int row = scale(28);
  const int summary_width = (std::max)(0, width - padding * 3 -
                                             scale(230));
  return {
      .summary = {padding, padding, summary_width, row},
      .filter = {padding * 2 + summary_width, padding,
                 (std::max)(0, width - padding * 3 - summary_width), row},
      .records = {padding, padding * 2 + row,
                  (std::max)(0, width - padding * 2),
                  (std::max)(0, height - padding * 3 - row)},
  };
}

}  // namespace listopad
