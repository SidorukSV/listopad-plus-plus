#include "listopad/document_map_cache.h"

#include <catch2/catch_test_macros.hpp>

using namespace listopad;

namespace {

DocumentMapLine line_with_style(const std::uint8_t style) {
  return {{.first_column = 1, .last_column = 4, .style = style}};
}

}  // namespace

TEST_CASE("document map line cache retains the unaffected prefix") {
  DocumentMapLineCache cache;
  cache.reset(5);
  for (std::size_t line = 0; line < 5; ++line) {
    cache.store(line, line_with_style(static_cast<std::uint8_t>(line)));
  }

  cache.invalidate_from(2, 5);

  REQUIRE(cache.line(0));
  REQUIRE(cache.line(1));
  CHECK(cache.line(0)->front().style == 0);
  CHECK(cache.line(1)->front().style == 1);
  CHECK(cache.line(2) == nullptr);
  CHECK(cache.line(4) == nullptr);
}

TEST_CASE("document map line cache handles inserted and deleted suffixes") {
  DocumentMapLineCache cache;
  cache.reset(4);
  cache.store(0, line_with_style(10));
  cache.store(1, line_with_style(11));
  cache.store(2, line_with_style(12));
  cache.store(3, line_with_style(13));

  cache.invalidate_from(2, 7);
  CHECK(cache.line_count() == 7);
  REQUIRE(cache.line(0));
  REQUIRE(cache.line(1));
  CHECK(cache.line(2) == nullptr);
  CHECK(cache.line(6) == nullptr);

  cache.store(2, line_with_style(20));
  cache.invalidate_from(1, 2);
  CHECK(cache.line_count() == 2);
  REQUIRE(cache.line(0));
  CHECK(cache.line(1) == nullptr);
}

TEST_CASE("document map line cache ignores stores outside its document") {
  DocumentMapLineCache cache;
  cache.reset(1);
  cache.store(4, line_with_style(99));

  CHECK(cache.line(4) == nullptr);
  CHECK(cache.line(0) == nullptr);
}

TEST_CASE("document map line cache stays sparse for very large documents") {
  DocumentMapLineCache cache;
  cache.reset(10'000'000);
  cache.store(0, line_with_style(1));
  cache.store(9'999'999, line_with_style(2));

  CHECK(cache.line_count() == 10'000'000);
  CHECK(cache.cached_line_count() == 2);
  REQUIRE(cache.line(0));
  CHECK(cache.line(5'000'000) == nullptr);
  REQUIRE(cache.line(9'999'999));
}
