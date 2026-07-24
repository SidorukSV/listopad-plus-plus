#include "listopad/document_map_geometry.h"

#include <catch2/catch_test_macros.hpp>

using namespace listopad;

TEST_CASE("document map uses the same coordinates for a medium document") {
  const DocumentMapGeometry geometry(899, 754);

  REQUIRE_FALSE(geometry.compressed());
  CHECK(geometry.line_height() == 1);
  CHECK(geometry.content_height() == 754);
  CHECK(geometry.line_top(597) == 597);
  CHECK(geometry.line_at(753) == 753);
  CHECK(geometry.line_at(898) == 753);
}

TEST_CASE("short document map stays anchored to the top") {
  const DocumentMapGeometry geometry(899, 16);

  REQUIRE_FALSE(geometry.compressed());
  CHECK(geometry.line_height() == 4);
  CHECK(geometry.content_height() == 64);
  CHECK(geometry.line_top(15) == 60);
  CHECK(geometry.line_bottom(15) == 64);
  CHECK(geometry.line_at(63) == 15);
  CHECK(geometry.line_at(800) == 15);
}

TEST_CASE("long document map remains compressed to the full height") {
  const DocumentMapGeometry geometry(899, 2000);

  REQUIRE(geometry.compressed());
  CHECK(geometry.content_height() == 899);
  CHECK(geometry.line_top(1000) == 449);
  CHECK(geometry.line_at(449) == 998);
  CHECK(geometry.line_at(898) == 1999);
  CHECK(geometry.line_bottom(1999) == 899);
}
