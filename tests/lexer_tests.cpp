#include "listopad/lexers.h"
#include "support/lexer_test_document.h"

#include <ILexer.h>
#include <SciLexer.h>
#include <Scintilla.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using TestDocument = listopad::test::LexerDocument;

struct LexerReleaser {
  void operator()(Scintilla::ILexer5* lexer) const noexcept {
    if (lexer) lexer->Release();
  }
};

using LexerPtr = std::unique_ptr<Scintilla::ILexer5, LexerReleaser>;

void configure_bsl(Scintilla::ILexer5& lexer) {
  lexer.WordListSet(
      0, "procedure endprocedure export if then else endif "
         "процедура конецпроцедуры экспорт если тогда иначе конецесли");
  lexer.WordListSet(1, "string number boolean строка число булево");
  lexer.WordListSet(2, "message string сообщить строка");
}

void configure_html(Scintilla::ILexer5& lexer) {
  lexer.WordListSet(1, "const let var function return class");
}

std::size_t first_style_difference(const TestDocument& expected,
                                   const TestDocument& actual) {
  const auto& expected_styles = expected.styles();
  const auto& actual_styles = actual.styles();
  const auto mismatch = std::mismatch(expected_styles.begin(),
                                      expected_styles.end(),
                                      actual_styles.begin(),
                                      actual_styles.end());
  return mismatch.first == expected_styles.end()
             ? std::string::npos
             : static_cast<std::size_t>(
                   std::distance(expected_styles.begin(), mismatch.first));
}

template <typename Factory, typename Configure>
void check_incremental_equivalence(const std::string& text, Factory factory,
                                   Configure configure,
                                   const int default_style) {
  TestDocument expected(text);
  LexerPtr full(factory());
  REQUIRE(full);
  configure(*full);
  full->Lex(0, expected.Length(), default_style, &expected);

  const std::array<std::size_t, 7> requested_starts{
      0,
      text.size() / 7,
      text.size() / 3,
      text.size() / 2,
      text.empty() ? 0 : text.size() - 1,
      text.find('\n') == std::string::npos ? 0 : text.find('\n') + 1,
      text.find("<style") == std::string::npos ? text.size() / 5
                                               : text.find("<style") + 3,
  };

  for (const std::size_t requested_start : requested_starts) {
    TestDocument incremental(text);
    LexerPtr partial(factory());
    REQUIRE(partial);
    configure(*partial);
    partial->Lex(0, incremental.Length(), default_style, &incremental);

    const auto start = static_cast<Sci_Position>(
        (std::min)(requested_start, text.size()));
    const int initial_style =
        start == 0
            ? default_style
            : static_cast<unsigned char>(incremental.StyleAt(start - 1));
    incremental.clear_styles_from(start);
    partial->Lex(static_cast<Sci_PositionU>(start),
                 incremental.Length() - start, initial_style, &incremental);

    const std::size_t mismatch =
        first_style_difference(expected, incremental);
    CAPTURE(requested_start, mismatch, text);
    CHECK(mismatch == std::string::npos);
  }
}

std::uint32_t next_value(std::uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  return state;
}

std::string generated_html(std::uint32_t seed) {
  static constexpr std::array<std::string_view, 12> fragments{
      "<div class=\"card\">текст</div>\n",
      "<style>.card { color: red; padding: 1px; }</style>\n",
      "<script>const answer = 42; if (answer) { alert(answer); }</script>\n",
      "<!-- комментарий со <style>ложным</style> тегом -->\n",
      "<input disabled data-value='x>y'>\n",
      "<custom-element aria-label=\"пример\"></custom-element>\n",
      "<style media=\"screen\">@media (min-width:1px){a:hover{opacity:.5}}</style>\n",
      "<script type=\"module\">let tag = \"</style>\";</script>\n",
      "<p title=\"кавычки &amp; сущности\">абзац</p>\n",
      "<template><span>{{ value }}</span></template>\n",
      "<br/><meta charset=\"utf-8\">\n",
      "обычный текст &lt; без тега\n",
  };

  std::string result = "<!DOCTYPE html>\n<html><head>\n";
  for (int index = 0; index < 10; ++index) {
    result.append(fragments[next_value(seed) % fragments.size()]);
  }
  result += "</head><body><section id=\"end\">конец</section></body></html>\n";
  return result;
}

std::string generated_bsl(std::uint32_t seed) {
  static constexpr std::array<std::string_view, 11> fragments{
      "&НаКлиенте\nПроцедура Тест(Параметр) Экспорт\n",
      "Если Параметр = 42 Тогда\nСообщить(\"Готово\");\nКонецЕсли;\n",
      "// комментарий с кавычкой \" и кириллицей\n",
      "Значение = '20260724';\n",
      "СтрокаТекста = \"двойная \"\"кавычка\"\"\";\n",
      "#Область Проверка\n#КонецОбласти\n",
      "Число = 1.25e-3 + 7;\n",
      "Массив[0] = НеизвестныйИдентификатор;\n",
      "\tСообщить(Строка(Параметр));\n",
      "СтрокаТекста = \"первая строка\n|вторая строка\";\n",
      "КонецПроцедуры\n",
  };

  std::string result;
  for (int index = 0; index < 12; ++index) {
    result.append(fragments[next_value(seed) % fragments.size()]);
  }
  return result;
}

}  // namespace

TEST_CASE("BSL lexer handles Cyrillic keywords, directives, strings, and comments") {
  TestDocument document(
      "&НаКлиенте\nПроцедура Тест(Параметр) Экспорт\n"
      "  // Комментарий\n  Сообщить(\"Готово\");\nКонецПроцедуры\n");
  Scintilla::ILexer5* lexer = listopad::create_bsl_lexer();
  REQUIRE(lexer);
  lexer->WordListSet(0, "procedure endprocedure export процедура конецпроцедуры экспорт");
  lexer->WordListSet(2, "message сообщить");
  lexer->Lex(0, document.Length(), listopad::BslDefault, &document);

  CHECK(document.style_at("&НаКлиенте") == listopad::BslAnnotation);
  CHECK(document.style_at("Процедура") == listopad::BslKeyword);
  CHECK(document.style_at("Параметр") == listopad::BslIdentifier);
  CHECK(document.style_at("// Комментарий") == listopad::BslComment);
  CHECK(document.style_at("Сообщить") == listopad::BslFunction);
  CHECK(document.style_at("\"Готово\"") == listopad::BslString);
  lexer->Release();
}

TEST_CASE("HTML lexer keeps markup, embedded CSS, and JavaScript styles separate") {
  TestDocument document(
      "<div class=\"card\"><style>.card { color: red; }</style>"
      "<script>const answer = 42;</script></div>");
  Scintilla::ILexer5* lexer = listopad::create_html_css_lexer();
  REQUIRE(lexer);
  lexer->WordListSet(1, "const");
  lexer->Lex(0, document.Length(), SCE_H_DEFAULT, &document);

  CHECK(document.style_at("div") == SCE_H_TAG);
  CHECK(document.style_at("class") == SCE_H_ATTRIBUTE);
  CHECK(document.style_at("\"card\"") == SCE_H_DOUBLESTRING);
  const unsigned char css_style = document.style_at("color");
  CHECK(css_style >= listopad::kEmbeddedCssStyleBase);
  CHECK(css_style != SCE_H_DOUBLESTRING);
  CHECK(document.style_at("const") == SCE_HJ_KEYWORD);
  CHECK(document.styling_position() == document.Length());
  lexer->Release();
}

TEST_CASE("HTML lexer recovers when incremental styling starts inside a tag") {
  TestDocument document(
      "<head>\n"
      "  <style>\n"
      "    .card { color: red; }\n"
      "  </style>\n"
      "</head>\n"
      "<body>\n"
      "  <helmet>\n"
      "    <link rel=\"preconnect\" href=\"https://example.test\">\n"
      "  </helmet>\n"
      "</body>\n");
  Scintilla::ILexer5* lexer = listopad::create_html_css_lexer();
  REQUIRE(lexer);
  lexer->Lex(0, document.Length(), SCE_H_DEFAULT, &document);

  const Sci_Position helmet = document.position_of("<helmet>");
  document.clear_styles_from(helmet);
  lexer->Lex(static_cast<Sci_PositionU>(helmet + 3),
             document.Length() - helmet - 3, SCE_H_DEFAULT, &document);

  CHECK((document.style_at("helmet") == SCE_H_TAG ||
         document.style_at("helmet") == SCE_H_TAGUNKNOWN));
  CHECK(document.style_at("link") == SCE_H_TAG);
  CHECK(document.style_at("body") == SCE_H_TAG);
  const unsigned char css_style = document.style_at("color");
  CHECK(css_style >= listopad::kEmbeddedCssStyleBase);
  lexer->Release();
}

TEST_CASE("HTML full and incremental lexing remain equivalent") {
  for (std::uint32_t seed = 1; seed <= 32; ++seed) {
    CAPTURE(seed);
    check_incremental_equivalence(
        generated_html(seed), listopad::create_html_css_lexer,
        configure_html, SCE_H_DEFAULT);
  }
}

TEST_CASE("HTML incremental lexing restores the enclosing style element") {
  const std::string text = "<style>a<s\n</style>";
  TestDocument expected(text);
  LexerPtr full(listopad::create_html_css_lexer());
  REQUIRE(full);
  configure_html(*full);
  full->Lex(0, expected.Length(), SCE_H_DEFAULT, &expected);

  TestDocument incremental(text);
  LexerPtr partial(listopad::create_html_css_lexer());
  REQUIRE(partial);
  configure_html(*partial);
  partial->Lex(0, incremental.Length(), SCE_H_DEFAULT, &incremental);
  const Sci_Position start = incremental.position_of("</style>") + 5;
  const int initial_style =
      static_cast<unsigned char>(incremental.StyleAt(start - 1));
  incremental.clear_styles_from(start);
  partial->Lex(static_cast<Sci_PositionU>(start),
               incremental.Length() - start, initial_style, &incremental);

  CHECK(first_style_difference(expected, incremental) == std::string::npos);
}

TEST_CASE("HTML incremental lexing preserves an earlier CSS overlay") {
  const std::string text = "<style>x;)</style><\nr2<";
  TestDocument expected(text);
  LexerPtr full(listopad::create_html_css_lexer());
  REQUIRE(full);
  configure_html(*full);
  full->Lex(0, expected.Length(), SCE_H_DEFAULT, &expected);

  TestDocument incremental(text);
  LexerPtr partial(listopad::create_html_css_lexer());
  REQUIRE(partial);
  configure_html(*partial);
  partial->Lex(0, incremental.Length(), SCE_H_DEFAULT, &incremental);
  const Sci_Position start = incremental.position_of("r2") + 1;
  const int initial_style =
      static_cast<unsigned char>(incremental.StyleAt(start - 1));
  incremental.clear_styles_from(start);
  partial->Lex(static_cast<Sci_PositionU>(start),
               incremental.Length() - start, initial_style, &incremental);

  CHECK(first_style_difference(expected, incremental) == std::string::npos);
  CHECK(incremental.style_at("x;)") >= listopad::kEmbeddedCssStyleBase);
}

TEST_CASE("BSL full and incremental lexing remain equivalent") {
  for (std::uint32_t seed = 1; seed <= 32; ++seed) {
    CAPTURE(seed);
    check_incremental_equivalence(
        generated_bsl(seed), listopad::create_bsl_lexer,
        configure_bsl, listopad::BslDefault);
  }
}

TEST_CASE("BSL incremental lexing restores a multi-line string state") {
  const std::string text =
      "СтрокаТекста = \"первая строка\n"
      "|вторая строка\";\n"
      "Сообщить(СтрокаТекста);\n";
  TestDocument expected(text);
  LexerPtr full(listopad::create_bsl_lexer());
  REQUIRE(full);
  configure_bsl(*full);
  full->Lex(0, expected.Length(), listopad::BslDefault, &expected);

  TestDocument incremental(text);
  LexerPtr partial(listopad::create_bsl_lexer());
  REQUIRE(partial);
  configure_bsl(*partial);
  partial->Lex(0, incremental.Length(), listopad::BslDefault, &incremental);
  const Sci_Position start = incremental.position_of("|вторая") + 3;
  const int initial_style =
      static_cast<unsigned char>(incremental.StyleAt(start - 1));
  incremental.clear_styles_from(start);
  partial->Lex(static_cast<Sci_PositionU>(start),
               incremental.Length() - start, initial_style, &incremental);

  CHECK(first_style_difference(expected, incremental) == std::string::npos);
  CHECK(incremental.style_at("|вторая") == listopad::BslString);
  CHECK(incremental.style_at("Сообщить") == listopad::BslFunction);
}
