#include "listopad/emmet_engine.h"

#include <catch2/catch_test_macros.hpp>

using namespace listopad;

TEST_CASE("Emmet expands HTML and produces ordered tabstops") {
  EmmetEngine engine;
  const EmmetExpansion result = engine.expand("a.btn", "html");
  REQUIRE(result.ok);
  REQUIRE(result.text.find("<a") != std::string::npos);
  REQUIRE(result.text.find("class=\"btn\"") != std::string::npos);
  REQUIRE(result.text.find('$') == std::string::npos);
  for (std::size_t index = 1; index < result.fields.size(); ++index) {
    const unsigned previous = result.fields[index - 1].index;
    const unsigned current = result.fields[index].index;
    if (current != 0) REQUIRE((previous != 0 && previous <= current));
  }
}

TEST_CASE("Emmet expands CSS abbreviations") {
  EmmetEngine engine;
  const EmmetExpansion result = engine.expand("m10", "css");
  REQUIRE(result.ok);
  REQUIRE(result.text.find("margin") != std::string::npos);
  REQUIRE(result.text.find("10px") != std::string::npos);
}

TEST_CASE("deleting an Emmet expansion cancels its stale tabstops") {
  EmmetEngine engine;
  EmmetExpansion expansion = engine.expand("span*30", "html");
  REQUIRE(expansion.ok);
  REQUIRE_FALSE(expansion.fields.empty());

  constexpr std::size_t insertion_point = 100;
  for (auto& field : expansion.fields) field.start += insertion_point;

  REQUIRE_FALSE(update_emmet_fields(
      expansion.fields, 0, insertion_point, expansion.text.size(), false));
  CHECK(expansion.fields.empty());
}

TEST_CASE("editing an Emmet field keeps following tabstops aligned") {
  std::vector<EmmetField> fields{{1, 10, 0}, {2, 20, 3}, {0, 30, 0}};

  REQUIRE(update_emmet_fields(fields, 0, 10, 4, true));
  CHECK(fields[0].length == 4);
  CHECK(fields[1].start == 24);
  CHECK(fields[2].start == 34);

  REQUIRE(update_emmet_fields(fields, 0, 11, 2, false));
  CHECK(fields[0].length == 2);
  CHECK(fields[1].start == 22);
  CHECK(fields[2].start == 32);
}
