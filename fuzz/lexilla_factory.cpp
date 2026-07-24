#include <ILexer.h>
#include <Lexilla.h>

#include <cstring>

#include <LexerModule.h>

extern const Lexilla::LexerModule lmCss;
extern const Lexilla::LexerModule lmHTML;

extern "C" Scintilla::ILexer5* LEXILLA_CALL CreateLexer(const char* name) {
  if (!name) return nullptr;
  if (std::strcmp(name, "css") == 0) return lmCss.Create();
  if (std::strcmp(name, "hypertext") == 0) return lmHTML.Create();
  return nullptr;
}
