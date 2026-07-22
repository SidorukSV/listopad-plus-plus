#include "listopad/language.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("language detection uses extension and shebang") {
  CHECK(listopad::detect_language(L"index.HTML").id == "html");
  CHECK(listopad::detect_language(L"script", "#!/usr/bin/env python3").id == "python");
  CHECK(listopad::detect_language(L"data.json").lexer == "json");
  CHECK(listopad::detect_language(L"ОбщийМодуль.bsl").id == "bsl");
  CHECK(listopad::detect_language(L"script.os").id == "onescript");
  CHECK(listopad::detect_language(L"script", "#!/usr/bin/env oscript").id == "onescript");
}

TEST_CASE("Emmet language capabilities are explicit") {
  REQUIRE(listopad::language_by_id("html"));
  CHECK(listopad::language_by_id("html")->emmet_markup);
  CHECK(listopad::language_by_id("css")->emmet_stylesheet);
  CHECK_FALSE(listopad::language_by_id("cpp")->emmet_markup);
}
