#include "listopad/formatter.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("JSON formatter indents and rejects invalid input") {
  const auto valid = listopad::format_text(listopad::FormatKind::Json, R"({"a":[1,2]})",
                                            {.indent = "  ", .eol = "\n"});
  REQUIRE(valid.ok);
  CHECK(valid.text.find("\n  \"a\"") != std::string::npos);
  const auto invalid = listopad::format_text(listopad::FormatKind::Json, R"({"a":})");
  CHECK_FALSE(invalid.ok);
  CHECK(invalid.line == 1);
}

TEST_CASE("XML formatter preserves a document without injecting declaration") {
  const auto result = listopad::format_text(listopad::FormatKind::Xml, "<root><item>1</item></root>",
                                             {.indent = "  ", .eol = "\n"});
  REQUIRE(result.ok);
  CHECK(result.text.find("<?xml") == std::string::npos);
  CHECK(result.text.find("\n  <item>") != std::string::npos);
}

TEST_CASE("HTML formatter accepts autonomous custom elements without dropping them") {
  const auto result = listopad::format_text(
      listopad::FormatKind::Html,
      "<x-dc><helmet><sc-if condition=\"ready\"><span>ok</span></sc-if></helmet></x-dc>",
      {.indent = "  ", .eol = "\n"});
  REQUIRE(result.ok);
  CHECK(result.text.find("<x-dc>") != std::string::npos);
  CHECK(result.text.find("<helmet>") != std::string::npos);
  CHECK(result.text.find("<sc-if") != std::string::npos);
  CHECK(result.text.find("</sc-if>") != std::string::npos);
}
