#include "listopad/lexers.h"
#include "support/lexer_test_document.h"

#include <ILexer.h>
#include <SciLexer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

constexpr std::size_t kControlBytes = 5;
constexpr std::size_t kMaximumDocumentBytes = 64 * 1024;

struct LexerReleaser {
  void operator()(Scintilla::ILexer5* lexer) const noexcept {
    if (lexer) lexer->Release();
  }
};

using LexerPtr = std::unique_ptr<Scintilla::ILexer5, LexerReleaser>;

std::uint32_t read_offset(const std::uint8_t* data) noexcept {
  return static_cast<std::uint32_t>(data[1]) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         (static_cast<std::uint32_t>(data[3]) << 16) |
         (static_cast<std::uint32_t>(data[4]) << 24);
}

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

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      const std::size_t size) {
  if (size < kControlBytes) return 0;

  const bool html = data[0] == 'H' || data[0] == 'h' || (data[0] & 1) != 0;
  const std::size_t text_size =
      (std::min)(size - kControlBytes, kMaximumDocumentBytes);
  const std::string text(
      reinterpret_cast<const char*>(data + kControlBytes), text_size);

  LexerPtr lexer(html ? listopad::create_html_css_lexer()
                      : listopad::create_bsl_lexer());
  if (!lexer) return 0;
  if (html) {
    configure_html(*lexer);
  } else {
    configure_bsl(*lexer);
  }

  const int default_style = html ? SCE_H_DEFAULT : listopad::BslDefault;
  listopad::test::LexerDocument expected(text);
  lexer->Lex(0, expected.Length(), default_style, &expected);

  LexerPtr incremental_lexer(html ? listopad::create_html_css_lexer()
                                  : listopad::create_bsl_lexer());
  if (!incremental_lexer) return 0;
  if (html) {
    configure_html(*incremental_lexer);
  } else {
    configure_bsl(*incremental_lexer);
  }

  listopad::test::LexerDocument incremental(text);
  incremental_lexer->Lex(0, incremental.Length(), default_style,
                         &incremental);

  const auto start = static_cast<Sci_Position>(
      text.empty() ? 0 : read_offset(data) % (text.size() + 1));
  const int initial_style =
      start == 0
          ? default_style
          : static_cast<unsigned char>(incremental.StyleAt(start - 1));
  const auto styles_before_incremental = incremental.styles();
  incremental.clear_styles_from(start);
  incremental_lexer->Lex(static_cast<Sci_PositionU>(start),
                         incremental.Length() - start, initial_style,
                         &incremental);

  // A crash here is a correctness finding: the same bytes received different
  // styles depending on whether Scintilla requested full or incremental work.
  if (expected.styles() != incremental.styles()) {
    const auto mismatch = std::mismatch(
        expected.styles().begin(), expected.styles().end(),
        incremental.styles().begin(), incremental.styles().end());
    const auto position = static_cast<std::size_t>(
        std::distance(expected.styles().begin(), mismatch.first));
    std::fprintf(
        stderr,
        "lexer differential mismatch: language=%s requested_start=%td "
        "position=%zu expected=%u before=%u actual=%u document_size=%zu\n",
        html ? "html" : "bsl", start, position,
        static_cast<unsigned char>(*mismatch.first),
        static_cast<unsigned char>(styles_before_incremental[position]),
        static_cast<unsigned char>(
            incremental.styles()[position]),
        text.size());
    std::abort();
  }
  return 0;
}
