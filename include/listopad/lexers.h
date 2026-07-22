#pragma once

namespace Scintilla {
class ILexer5;
}

namespace listopad {

enum BslStyle {
  BslDefault = 0,
  BslComment = 1,
  BslNumber = 2,
  BslString = 3,
  BslDate = 4,
  BslKeyword = 5,
  BslPreprocessor = 6,
  BslAnnotation = 7,
  BslOperator = 8,
  BslIdentifier = 9,
  BslType = 10,
  BslFunction = 11,
};

inline constexpr int kEmbeddedCssStyleBase = 160;

Scintilla::ILexer5* create_bsl_lexer();
Scintilla::ILexer5* create_html_css_lexer();

}  // namespace listopad
