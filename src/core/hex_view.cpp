#include "listopad/hex_view.h"

#include <algorithm>
#include <array>
#include <limits>

namespace listopad {
namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

unsigned hex_value(const char value) {
  if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
  if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a' + 10);
  if (value >= 'A' && value <= 'F') return static_cast<unsigned>(value - 'A' + 10);
  return 16;
}

unsigned char fold_ascii(const unsigned char value) {
  return value >= 'A' && value <= 'Z'
      ? static_cast<unsigned char>(value - 'A' + 'a')
      : value;
}

bool equal_byte(const std::byte left, const std::byte right,
                const bool ascii_case_insensitive) {
  const unsigned char lhs = std::to_integer<unsigned char>(left);
  const unsigned char rhs = std::to_integer<unsigned char>(right);
  return ascii_case_insensitive ? fold_ascii(lhs) == fold_ascii(rhs) : lhs == rhs;
}

std::optional<std::size_t> find_range(
    const std::span<const std::byte> subject,
    const std::span<const std::byte> pattern,
    const std::size_t begin, const std::size_t end,
    const bool ascii_case_insensitive, const std::stop_token stop) {
  if (pattern.size() > end - begin) return std::nullopt;
  const std::size_t last = end - pattern.size();
  for (std::size_t offset = begin; offset <= last; ++offset) {
    if ((offset & 0xffffu) == 0 && stop.stop_requested()) return std::nullopt;
    bool matches = true;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
      if (!equal_byte(subject[offset + index], pattern[index],
                      ascii_case_insensitive)) {
        matches = false;
        break;
      }
    }
    if (matches) return offset;
  }
  return std::nullopt;
}

}  // namespace

std::uint64_t hex_row_count(const std::uint64_t size) noexcept {
  return size / kHexBytesPerRow + (size % kHexBytesPerRow != 0);
}

unsigned hex_offset_width(const std::uint64_t size) noexcept {
  std::uint64_t maximum = size > 0 ? size - 1 : 0;
  unsigned digits = 1;
  while (maximum >= 16) {
    maximum /= 16;
    ++digits;
  }
  return std::clamp(digits, 8u, 16u);
}

int hex_scroll_position(const std::uint64_t row,
                        const std::uint64_t maximum_row,
                        const int maximum_position) noexcept {
  if (maximum_row == 0 || maximum_position <= 0) return 0;
  const std::uint64_t clamped_row = std::min(row, maximum_row);
  if (maximum_row == static_cast<std::uint64_t>(maximum_position)) {
    return static_cast<int>(clamped_row);
  }
  return static_cast<int>(
      static_cast<long double>(clamped_row) * maximum_position /
      maximum_row);
}

std::uint64_t hex_row_from_scroll_position(
    const int position, const std::uint64_t maximum_row,
    const int maximum_position) noexcept {
  if (maximum_row == 0 || maximum_position <= 0) return 0;
  const int clamped_position =
      std::clamp(position, 0, maximum_position);
  if (maximum_row == static_cast<std::uint64_t>(maximum_position)) {
    return static_cast<std::uint64_t>(clamped_position);
  }
  return static_cast<std::uint64_t>(
      static_cast<long double>(clamped_position) * maximum_row /
      maximum_position);
}

HexRowText format_hex_row(const std::span<const std::byte> row,
                          const std::uint64_t offset,
                          const unsigned requested_offset_width) {
  HexRowText result;
  const unsigned offset_width = std::clamp(requested_offset_width, 1u, 16u);
  result.offset.assign(offset_width, '0');
  std::uint64_t remaining = offset;
  for (unsigned index = 0; index < offset_width; ++index) {
    result.offset[offset_width - index - 1] = kHexDigits[remaining & 0xfu];
    remaining >>= 4;
  }

  result.hex.reserve(kHexBytesPerRow * 3 - 1);
  result.ascii.reserve(kHexBytesPerRow);
  for (std::size_t index = 0; index < kHexBytesPerRow; ++index) {
    if (index < row.size()) {
      const unsigned value = std::to_integer<unsigned>(row[index]);
      result.hex.push_back(kHexDigits[value >> 4]);
      result.hex.push_back(kHexDigits[value & 0xf]);
      result.ascii.push_back(value >= 0x20 && value <= 0x7e
                                 ? static_cast<char>(value)
                                 : '.');
    } else {
      result.hex.append("  ");
      result.ascii.push_back(' ');
    }
    if (index + 1 < kHexBytesPerRow) result.hex.push_back(' ');
  }
  return result;
}

std::optional<std::vector<std::byte>> parse_hex_pattern(
    const std::string_view pattern) {
  std::string digits;
  digits.reserve(pattern.size());
  for (const char value : pattern) {
    if (value == ' ' || value == '\t' || value == '\r' || value == '\n') continue;
    if (hex_value(value) >= 16) return std::nullopt;
    digits.push_back(value);
  }
  if (digits.empty() || digits.size() % 2 != 0) return std::nullopt;

  std::vector<std::byte> bytes;
  bytes.reserve(digits.size() / 2);
  for (std::size_t index = 0; index < digits.size(); index += 2) {
    bytes.push_back(std::byte{static_cast<unsigned char>(
        (hex_value(digits[index]) << 4) | hex_value(digits[index + 1]))});
  }
  return bytes;
}

SearchOneResult find_next_bytes(
    const std::span<const std::byte> subject,
    const std::span<const std::byte> pattern,
    const std::size_t start, const bool wrap,
    const bool ascii_case_insensitive, const std::stop_token stop) {
  SearchOneResult result;
  if (pattern.empty()) {
    result.ok = true;
    return result;
  }
  const std::size_t safe_start = std::min(start, subject.size());
  if (const auto found = find_range(subject, pattern, safe_start, subject.size(),
                                    ascii_case_insensitive, stop)) {
    result.ok = true;
    result.found = true;
    result.match = {*found, pattern.size()};
    return result;
  }
  if (stop.stop_requested()) {
    result.error = "Search cancelled";
    return result;
  }
  if (wrap && safe_start > 0) {
    if (const auto found = find_range(subject, pattern, 0, safe_start,
                                      ascii_case_insensitive, stop)) {
      result.ok = true;
      result.found = true;
      result.match = {*found, pattern.size()};
      return result;
    }
  }
  if (stop.stop_requested()) {
    result.error = "Search cancelled";
    return result;
  }
  result.ok = true;
  return result;
}

}  // namespace listopad
