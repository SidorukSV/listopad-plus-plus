#include "listopad/language.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("language detection uses extension and shebang") {
  CHECK(listopad::detect_language(L"index.HTML").id == "html");
  CHECK(listopad::detect_language(L"script", "#!/usr/bin/env python3").id == "python");
  CHECK(listopad::detect_language(L"data.json").lexer == "json");
  CHECK(listopad::detect_language(L"ОбщийМодуль.bsl").id == "bsl");
  CHECK(listopad::detect_language(L"script.os").id == "onescript");
  CHECK(listopad::detect_language(L"script", "#!/usr/bin/env oscript").id == "onescript");
  CHECK(listopad::detect_language(L"notes.TXT").id == "text");
  CHECK(listopad::detect_language(L"application.log").id == "text");
}

TEST_CASE("Emmet language capabilities are explicit") {
  REQUIRE(listopad::language_by_id("html"));
  CHECK(listopad::language_by_id("html")->emmet_markup);
  CHECK(listopad::language_by_id("css")->emmet_stylesheet);
  CHECK_FALSE(listopad::language_by_id("cpp")->emmet_markup);
}

TEST_CASE("language extension metadata is round trippable") {
  for (const auto& language : listopad::languages()) {
    REQUIRE_FALSE(language.default_extension.empty());
    REQUIRE(listopad::language_by_extension(language.default_extension));
    CHECK(listopad::language_by_extension(language.default_extension)->id == language.id);
    for (const auto& extension : language.extensions) {
      REQUIRE(listopad::language_by_extension(extension));
      CHECK(listopad::language_by_extension(extension)->id == language.id);
    }
  }
  CHECK(listopad::language_by_extension("JSONC")->id == "json");
  CHECK(listopad::language_by_extension(".patch")->id == "diff");
  CHECK(listopad::language_by_extension(".unknown") == nullptr);
}

TEST_CASE("default extension is appended only when the name has no explicit suffix") {
  CHECK(listopad::append_default_extension(L"new-file", "json") ==
        std::filesystem::path(L"new-file.json"));
  CHECK(listopad::append_default_extension(L"new-file.custom", "json") ==
        std::filesystem::path(L"new-file.custom"));
  CHECK(listopad::append_default_extension(L".gitignore", "text") ==
        std::filesystem::path(L".gitignore"));
  CHECK(listopad::append_default_extension(L"new-file", "raw-lexer") ==
        std::filesystem::path(L"new-file"));
}
