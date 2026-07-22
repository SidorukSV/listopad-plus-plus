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
