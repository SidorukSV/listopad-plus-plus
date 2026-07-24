#include "listopad/lexers.h"

#include <cassert>
#include <cctype>
#include <string>
#include <string_view>

#include <ILexer.h>
#include <SciLexer.h>
#include <Scintilla.h>

#include <LexAccessor.h>
#include <Accessor.h>
#include <CharacterSet.h>
#include <LexerModule.h>
#include <StyleContext.h>
#include <WordList.h>

using namespace Lexilla;

namespace listopad {
namespace {

bool identifier_start(const int ch) noexcept {
  return ch >= 0x80 || ch == '_' || (ch < 0x80 && std::isalpha(static_cast<unsigned char>(ch)));
}

bool identifier_continue(const int ch) noexcept {
  return ch >= 0x80 || ch == '_' || (ch < 0x80 && std::isalnum(static_cast<unsigned char>(ch)));
}

bool number_continue(const int previous, const int ch) noexcept {
  return IsADigit(ch) || ch == '.' || ch == 'e' || ch == 'E' ||
         ((ch == '+' || ch == '-') && (previous == 'e' || previous == 'E'));
}

bool bsl_operator(const int ch) noexcept {
  return ch < 0x80 && std::string_view("+-*/%=<>();,.[]?:").find(static_cast<char>(ch)) != std::string_view::npos;
}

void append_utf8(std::string& result, const char32_t codepoint) {
  if (codepoint <= 0x7f) {
    result.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ff) {
    result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint <= 0xffff) {
    result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

std::string bsl_case_fold(const std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size();) {
    const unsigned char first = static_cast<unsigned char>(text[index]);
    char32_t codepoint = first;
    std::size_t width = 1;
    if ((first & 0xe0) == 0xc0 && index + 1 < text.size()) {
      codepoint = ((first & 0x1f) << 6) |
                  (static_cast<unsigned char>(text[index + 1]) & 0x3f);
      width = 2;
    } else if ((first & 0xf0) == 0xe0 && index + 2 < text.size()) {
      codepoint = ((first & 0x0f) << 12) |
                  ((static_cast<unsigned char>(text[index + 1]) & 0x3f) << 6) |
                  (static_cast<unsigned char>(text[index + 2]) & 0x3f);
      width = 3;
    } else if ((first & 0xf8) == 0xf0 && index + 3 < text.size()) {
      codepoint = ((first & 0x07) << 18) |
                  ((static_cast<unsigned char>(text[index + 1]) & 0x3f) << 12) |
                  ((static_cast<unsigned char>(text[index + 2]) & 0x3f) << 6) |
                  (static_cast<unsigned char>(text[index + 3]) & 0x3f);
      width = 4;
    }
    if (codepoint >= U'A' && codepoint <= U'Z') codepoint += U'a' - U'A';
    if (codepoint >= U'А' && codepoint <= U'Я') codepoint += U'а' - U'А';
    if (codepoint == U'Ё') codepoint = U'ё';
    append_utf8(result, codepoint);
    index += width;
  }
  return result;
}

void classify_identifier(StyleContext& context, WordList* keyword_lists[]) {
  std::string token;
  context.GetCurrentString(token, StyleContext::Transform::none);
  token = bsl_case_fold(token);
  if (keyword_lists[0]->InList(token.c_str())) {
    context.ChangeState(BslKeyword);
  } else if (keyword_lists[1]->InList(token.c_str())) {
    context.ChangeState(BslType);
  } else if (keyword_lists[2]->InList(token.c_str())) {
    context.ChangeState(BslFunction);
  }
}

void colourise_bsl(Sci_PositionU requested_start,
                   const Sci_Position requested_length,
                   const int /*initial_style*/,
                   WordList* keyword_lists[], Accessor& styler) {
  const Sci_Position requested_end =
      static_cast<Sci_Position>(requested_start) + requested_length;
  const Sci_Position start =
      styler.LineStart(styler.GetLine(static_cast<Sci_Position>(requested_start)));
  const Sci_Position length = (std::max)(Sci_Position{}, requested_end - start);
  // Rewind to a physical line boundary, but preserve a state that legitimately
  // crosses it (notably a multi-line string). A line comment styles the text
  // before the newline, so the newline itself already provides BslDefault.
  const int restored_style =
      start == 0
          ? BslDefault
          : static_cast<unsigned char>(styler.StyleAt(start - 1));
  StyleContext context(static_cast<Sci_PositionU>(start), length,
                       restored_style, styler);
  for (; context.More(); context.Forward()) {
    switch (context.state) {
      case BslComment:
        if (context.MatchLineEnd()) context.SetState(BslDefault);
        break;
      case BslString:
        if (context.ch == '"') {
          if (context.chNext == '"') context.Forward();
          else context.ForwardSetState(BslDefault);
        }
        break;
      case BslDate:
        if (context.ch == '\'') context.ForwardSetState(BslDefault);
        break;
      case BslNumber:
        if (!number_continue(context.chPrev, context.ch)) context.SetState(BslDefault);
        break;
      case BslIdentifier:
        if (!identifier_continue(context.ch)) {
          classify_identifier(context, keyword_lists);
          context.SetState(BslDefault);
        }
        break;
      case BslPreprocessor:
      case BslAnnotation:
        if (!identifier_continue(context.ch)) context.SetState(BslDefault);
        break;
      case BslOperator:
        context.SetState(BslDefault);
        break;
      default:
        break;
    }

    if (context.state != BslDefault) continue;
    if (context.Match('/', '/')) {
      context.SetState(BslComment);
      context.Forward();
    } else if (context.ch == '"') {
      context.SetState(BslString);
    } else if (context.ch == '\'') {
      context.SetState(BslDate);
    } else if (IsADigit(context.ch)) {
      context.SetState(BslNumber);
    } else if (context.ch == '#') {
      context.SetState(BslPreprocessor);
    } else if (context.ch == '&') {
      context.SetState(BslAnnotation);
    } else if (identifier_start(context.ch)) {
      context.SetState(BslIdentifier);
    } else if (bsl_operator(context.ch)) {
      context.SetState(BslOperator);
    }
  }
  if (context.state == BslIdentifier) classify_identifier(context, keyword_lists);
  context.Complete();
}

const char* const bsl_word_lists[] = {
    "BSL keywords", "Built-in values and types", "Global functions", nullptr};

const LexerModule bsl_module(SCLEX_CONTAINER, colourise_bsl, "bsl", nullptr, bsl_word_lists);

}  // namespace

Scintilla::ILexer5* create_bsl_lexer() { return bsl_module.Create(); }

}  // namespace listopad
