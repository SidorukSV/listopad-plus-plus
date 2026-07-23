#pragma once

#include "listopad/search.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace listopad {

inline constexpr std::size_t kHexBytesPerRow = 16;

struct HexRowText {
  std::string offset;
  std::string hex;
  std::string ascii;
};

[[nodiscard]] std::uint64_t hex_row_count(std::uint64_t size) noexcept;
[[nodiscard]] unsigned hex_offset_width(std::uint64_t size) noexcept;
[[nodiscard]] HexRowText format_hex_row(std::span<const std::byte> row,
                                        std::uint64_t offset,
                                        unsigned offset_width);
[[nodiscard]] std::optional<std::vector<std::byte>> parse_hex_pattern(
    std::string_view pattern);
[[nodiscard]] SearchOneResult find_next_bytes(
    std::span<const std::byte> subject, std::span<const std::byte> pattern,
    std::size_t start, bool wrap, bool ascii_case_insensitive = false,
    std::stop_token stop = {});

}  // namespace listopad
