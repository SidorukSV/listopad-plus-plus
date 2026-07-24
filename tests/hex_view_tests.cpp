#include "listopad/hex_view.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <string>

using namespace listopad;

namespace {

std::vector<std::byte> bytes(const std::initializer_list<unsigned> values) {
  std::vector<std::byte> result;
  result.reserve(values.size());
  for (const unsigned value : values) {
    result.push_back(std::byte{static_cast<unsigned char>(value)});
  }
  return result;
}

std::span<const std::byte> byte_span(const std::string& value) {
  return std::as_bytes(std::span(value.data(), value.size()));
}

}  // namespace

TEST_CASE("hex rows format fixed columns and non printable bytes") {
  const auto input = bytes({
      0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x00, 0x1f, 0x20,
      0x7e, 0x7f, 0xc3, 0x90, 0x31, 0x32, 0x33, 0x34});
  const HexRowText row = format_hex_row(input, 0xa0, 8);
  CHECK(row.offset == "000000A0");
  CHECK(row.hex ==
        "48 65 6C 6C 6F 00 1F 20 7E 7F C3 90 31 32 33 34");
  CHECK(row.ascii == "Hello.. ~...1234");
}

TEST_CASE("short hex rows keep the ASCII and hex columns aligned") {
  const auto input = bytes({0xde, 0xad, 0xbe});
  const HexRowText row = format_hex_row(input, 0, 8);
  CHECK(row.hex.size() == kHexBytesPerRow * 3 - 1);
  CHECK(row.hex.starts_with("DE AD BE"));
  CHECK(row.ascii == "..." + std::string(13, ' '));
}

TEST_CASE("hex row counts and offset widths cover boundaries") {
  CHECK(hex_row_count(0) == 0);
  CHECK(hex_row_count(1) == 1);
  CHECK(hex_row_count(15) == 1);
  CHECK(hex_row_count(16) == 1);
  CHECK(hex_row_count(17) == 2);
  CHECK(hex_row_count(std::numeric_limits<std::uint64_t>::max()) ==
        std::numeric_limits<std::uint64_t>::max() / 16 + 1);
  CHECK(hex_offset_width(0) == 8);
  CHECK(hex_offset_width(0x1'0000'0001ull) == 9);
}

TEST_CASE("hex scrollbar projection is exact when possible and stable when scaled") {
  CHECK(hex_scroll_position(0, 100, 100) == 0);
  CHECK(hex_scroll_position(37, 100, 100) == 37);
  CHECK(hex_scroll_position(200, 100, 100) == 100);
  CHECK(hex_row_from_scroll_position(37, 100, 100) == 37);
  CHECK(hex_row_from_scroll_position(-1, 100, 100) == 0);

  constexpr std::uint64_t large_maximum = 1ull << 40;
  constexpr int scroll_maximum = 1'000'000;
  CHECK(hex_scroll_position(large_maximum, large_maximum,
                            scroll_maximum) == scroll_maximum);
  CHECK(hex_row_from_scroll_position(scroll_maximum, large_maximum,
                                     scroll_maximum) == large_maximum);
  std::uint64_t previous = 0;
  for (int position = 0; position <= scroll_maximum; position += 10'000) {
    const std::uint64_t row = hex_row_from_scroll_position(
        position, large_maximum, scroll_maximum);
    CHECK(row >= previous);
    CHECK(hex_scroll_position(row, large_maximum, scroll_maximum) <=
          position);
    previous = row;
  }
}

TEST_CASE("hex patterns accept spaced and compact notation") {
  REQUIRE(parse_hex_pattern("DE AD BE EF"));
  CHECK(*parse_hex_pattern("DE AD BE EF") == bytes({0xde, 0xad, 0xbe, 0xef}));
  REQUIRE(parse_hex_pattern("deadbeef"));
  CHECK(*parse_hex_pattern("deadbeef") == bytes({0xde, 0xad, 0xbe, 0xef}));
  CHECK_FALSE(parse_hex_pattern(""));
  CHECK_FALSE(parse_hex_pattern("ABC"));
  CHECK_FALSE(parse_hex_pattern("DE AD BG"));
}

TEST_CASE("byte search handles NUL invalid UTF wrap and cancellation") {
  const auto subject = bytes({0xff, 0x00, 0x41, 0x62, 0x43, 0xff, 0x00});
  const auto binary = bytes({0xff, 0x00});
  const SearchOneResult first = find_next_bytes(subject, binary, 0, false);
  REQUIRE(first.ok);
  REQUIRE(first.found);
  CHECK(first.match.start == 0);

  const SearchOneResult wrapped = find_next_bytes(subject, binary, 2, true);
  REQUIRE(wrapped.ok);
  REQUIRE(wrapped.found);
  CHECK(wrapped.match.start == 5);
  const SearchOneResult wrapped_again = find_next_bytes(subject, binary, 7, true);
  REQUIRE(wrapped_again.found);
  CHECK(wrapped_again.match.start == 0);

  const std::string mixed = "aBc";
  const SearchOneResult folded =
      find_next_bytes(subject, byte_span(mixed), 0, false, true);
  REQUIRE(folded.found);
  CHECK(folded.match.start == 2);

  std::stop_source source;
  source.request_stop();
  const SearchOneResult cancelled =
      find_next_bytes(subject, binary, 0, false, false, source.get_token());
  CHECK_FALSE(cancelled.ok);
  CHECK(cancelled.error == "Search cancelled");
}
