#include "listopad/lexers.h"

#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>
#include <Scintilla.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include <vector>

namespace listopad {
namespace {

class CssSubDocument final : public Scintilla::IDocument {
 public:
  CssSubDocument(Scintilla::IDocument* parent, const Sci_Position offset,
                 const Sci_Position length)
      : parent_(parent), offset_(offset), length_(length),
        first_line_(parent->LineFromPosition(offset)) {}

  int SCI_METHOD Version() const override { return parent_->Version(); }
  void SCI_METHOD SetErrorStatus(const int status) override { parent_->SetErrorStatus(status); }
  Sci_Position SCI_METHOD Length() const override { return length_; }
  void SCI_METHOD GetCharRange(char* buffer, const Sci_Position position,
                               const Sci_Position length) const override {
    parent_->GetCharRange(buffer, offset_ + position, length);
  }
  char SCI_METHOD StyleAt(const Sci_Position position) const override {
    const unsigned char style = static_cast<unsigned char>(parent_->StyleAt(offset_ + position));
    return style >= kEmbeddedCssStyleBase && style < kEmbeddedCssStyleBase + 64
               ? static_cast<char>(style - kEmbeddedCssStyleBase)
               : static_cast<char>(style);
  }
  Sci_Position SCI_METHOD LineFromPosition(const Sci_Position position) const override {
    return parent_->LineFromPosition(offset_ + position) - first_line_;
  }
  Sci_Position SCI_METHOD LineStart(const Sci_Position line) const override {
    return relative_position(parent_->LineStart(first_line_ + line));
  }
  int SCI_METHOD GetLevel(const Sci_Position line) const override {
    return parent_->GetLevel(first_line_ + line);
  }
  int SCI_METHOD SetLevel(const Sci_Position line, const int level) override {
    return parent_->SetLevel(first_line_ + line, level);
  }
  int SCI_METHOD GetLineState(const Sci_Position line) const override {
    return parent_->GetLineState(first_line_ + line);
  }
  int SCI_METHOD SetLineState(const Sci_Position line, const int state) override {
    return parent_->SetLineState(first_line_ + line, state);
  }
  void SCI_METHOD StartStyling(const Sci_Position position) override {
    parent_->StartStyling(offset_ + position);
  }
  bool SCI_METHOD SetStyleFor(const Sci_Position length, const char style) override {
    return parent_->SetStyleFor(length, map_style(style));
  }
  bool SCI_METHOD SetStyles(const Sci_Position length, const char* styles) override {
    std::vector<char> mapped(static_cast<std::size_t>((std::max)(length, Sci_Position{})));
    std::transform(styles, styles + mapped.size(), mapped.begin(), map_style);
    return parent_->SetStyles(length, mapped.data());
  }
  void SCI_METHOD DecorationSetCurrentIndicator(const int indicator) override {
    parent_->DecorationSetCurrentIndicator(indicator);
  }
  void SCI_METHOD DecorationFillRange(const Sci_Position position, const int value,
                                      const Sci_Position fill_length) override {
    parent_->DecorationFillRange(offset_ + position, value, fill_length);
  }
  void SCI_METHOD ChangeLexerState(const Sci_Position start, const Sci_Position end) override {
    parent_->ChangeLexerState(offset_ + start, offset_ + end);
  }
  int SCI_METHOD CodePage() const override { return parent_->CodePage(); }
  bool SCI_METHOD IsDBCSLeadByte(const char ch) const override {
    return parent_->IsDBCSLeadByte(ch);
  }
  const char* SCI_METHOD BufferPointer() override { return parent_->BufferPointer() + offset_; }
  int SCI_METHOD GetLineIndentation(const Sci_Position line) override {
    return parent_->GetLineIndentation(first_line_ + line);
  }
  Sci_Position SCI_METHOD LineEnd(const Sci_Position line) const override {
    return relative_position(parent_->LineEnd(first_line_ + line));
  }
  Sci_Position SCI_METHOD GetRelativePosition(const Sci_Position position_start,
                                              const Sci_Position character_offset) const override {
    return relative_position(parent_->GetRelativePosition(offset_ + position_start,
                                                          character_offset));
  }
  int SCI_METHOD GetCharacterAndWidth(const Sci_Position position,
                                      Sci_Position* width) const override {
    return parent_->GetCharacterAndWidth(offset_ + position, width);
  }

 private:
  static char map_style(const char style) {
    return static_cast<char>(kEmbeddedCssStyleBase + static_cast<unsigned char>(style));
  }
  Sci_Position relative_position(const Sci_Position absolute) const {
    return (std::clamp)(absolute - offset_, Sci_Position{}, length_);
  }

  Scintilla::IDocument* parent_;
  Sci_Position offset_;
  Sci_Position length_;
  Sci_Position first_line_;
};

bool ascii_equal_at(const char* text, const Sci_Position length, const Sci_Position position,
                    const std::string_view expected) {
  if (position < 0 || position + static_cast<Sci_Position>(expected.size()) > length) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto actual = static_cast<unsigned char>(text[position + static_cast<Sci_Position>(index)]);
    if (std::tolower(actual) != expected[index]) return false;
  }
  return true;
}

bool tag_boundary(const char value) noexcept {
  return value == '>' || value == '/' || value == '\0' ||
         std::isspace(static_cast<unsigned char>(value)) != 0;
}

Sci_Position tag_end(const char* text, const Sci_Position length, Sci_Position position) {
  char quote = 0;
  for (; position < length; ++position) {
    const char ch = text[position];
    if (quote) {
      if (ch == quote) quote = 0;
    } else if (ch == '\'' || ch == '"') {
      quote = ch;
    } else if (ch == '>') {
      return position + 1;
    }
  }
  return length;
}

Sci_Position find_closing_style(const char* text, const Sci_Position length,
                                Sci_Position position) {
  for (; position + 7 <= length; ++position) {
    if (text[position] == '<' && ascii_equal_at(text, length, position, "</style") &&
        tag_boundary(position + 7 < length ? text[position + 7] : '\0')) {
      return position;
    }
  }
  return length;
}

Sci_Position expand_start_for_embedded_css(Scintilla::IDocument* document,
                                           const char* text,
                                           const Sci_Position document_length,
                                           const Sci_Position line_start) {
  for (Sci_Position position = 0; position + 6 <= document_length; ++position) {
    if (text[position] != '<' ||
        !ascii_equal_at(text, document_length, position, "<style") ||
        !tag_boundary(position + 6 < document_length ? text[position + 6]
                                                     : '\0')) {
      continue;
    }
    const unsigned char tag_style =
        static_cast<unsigned char>(document->StyleAt(position + 1));
    if (tag_style != SCE_H_TAG && tag_style != SCE_H_TAGUNKNOWN) continue;

    const Sci_Position content_start =
        tag_end(text, document_length, position + 6);
    const Sci_Position content_end =
        find_closing_style(text, document_length, content_start);
    const Sci_Position closing_end =
        content_end < document_length
            ? tag_end(text, document_length, content_end + 7)
            : document_length;
    if (line_start >= content_start && line_start < closing_end) {
      return position;
    }
    position = content_end;
  }
  return line_start;
}

class HtmlCssLexer final : public Scintilla::ILexer5 {
 public:
  HtmlCssLexer() : html_(CreateLexer("hypertext")), css_(CreateLexer("css")) {
    if (css_) {
      css_->WordListSet(0, "color background border margin padding font display position width height top right bottom left opacity overflow content transform transition animation");
      css_->WordListSet(1, "active checked disabled empty enabled first-child first-of-type focus hover last-child last-of-type not nth-child root visited");
      css_->WordListSet(2, "align-items align-content box-sizing flex flex-direction gap grid line-height max-height max-width min-height min-width object-fit text-align text-decoration white-space z-index");
      css_->WordListSet(3, "appearance backdrop-filter background-color border-radius box-shadow caret-color column-gap cursor filter justify-content pointer-events resize scrollbar-color scrollbar-width user-select");
      css_->WordListSet(4, "after before first-letter first-line placeholder selection");
    }
  }
  ~HtmlCssLexer() {
    if (html_) html_->Release();
    if (css_) css_->Release();
  }

  int SCI_METHOD Version() const override { return html_->Version(); }
  void SCI_METHOD Release() override { delete this; }
  const char* SCI_METHOD PropertyNames() override { return html_->PropertyNames(); }
  int SCI_METHOD PropertyType(const char* name) override { return html_->PropertyType(name); }
  const char* SCI_METHOD DescribeProperty(const char* name) override {
    return html_->DescribeProperty(name);
  }
  Sci_Position SCI_METHOD PropertySet(const char* key, const char* value) override {
    if (css_) css_->PropertySet(key, value);
    return html_->PropertySet(key, value);
  }
  const char* SCI_METHOD DescribeWordListSets() override { return html_->DescribeWordListSets(); }
  Sci_Position SCI_METHOD WordListSet(const int index, const char* words) override {
    return html_->WordListSet(index, words);
  }
  void SCI_METHOD Lex(const Sci_PositionU start, const Sci_Position length, int initial_style,
                      Scintilla::IDocument* document) override {
    const Sci_Position document_length = document->Length();
    if (document_length <= 0) return;
    const Sci_Position requested_start =
        (std::min)(document_length, static_cast<Sci_Position>(start));
    const Sci_Position requested_end = (std::min)(document_length, requested_start + length);
    const char* text = document->BufferPointer();
    // Scintilla may ask to recolour from the middle of a tag after an edit. The
    // stock HTML lexer expects the state immediately before its start position;
    // rewinding to the physical line preserves that context and prevents the
    // rest of the tag (and following tags) from falling back to default text.
    Sci_Position expanded_start = document->LineStart(
        document->LineFromPosition(requested_start));
    // Embedded CSS replaces the HTML lexer's raw-text styles, so the style
    // immediately before a later physical line cannot restore the underlying
    // HTML state. If the line begins inside a <style> body or its closing tag,
    // replay the owning opening tag as the nearest reliable checkpoint.
    expanded_start = expand_start_for_embedded_css(
        document, text, document_length, expanded_start);
    if (expanded_start > 0) {
      initial_style = static_cast<unsigned char>(
          document->StyleAt(expanded_start - 1));
    } else {
      initial_style = SCE_H_DEFAULT;
    }
    // CSS styles are overlaid by this lexer and do not describe an HTML state.
    // To LexHTML, the contents of <style> are ordinary raw text.
    if (initial_style >= kEmbeddedCssStyleBase) initial_style = SCE_H_DEFAULT;
    html_->Lex(static_cast<Sci_PositionU>(expanded_start),
               requested_end - expanded_start, initial_style, document);
    if (!css_) return;

    for (Sci_Position position = 0; position + 6 <= document_length; ++position) {
      if (text[position] != '<' || !ascii_equal_at(text, document_length, position, "<style") ||
          !tag_boundary(position + 6 < document_length ? text[position + 6] : '\0')) continue;
      const unsigned char tag_style = static_cast<unsigned char>(document->StyleAt(position + 1));
      if (tag_style != SCE_H_TAG && tag_style != SCE_H_TAGUNKNOWN) continue;
      const Sci_Position content_start = tag_end(text, document_length, position + 6);
      const Sci_Position content_end = find_closing_style(text, document_length, content_start);
      // LexHTML may rewind farther than the start it was given while restoring
      // malformed-tag state. Reapply every CSS overlay after it runs so an
      // earlier, otherwise untouched <style> block cannot lose its colours.
      if (content_end > content_start) {
        CssSubDocument css_document(document, content_start, content_end - content_start);
        css_->Lex(0, content_end - content_start, SCE_CSS_DEFAULT, &css_document);
      }
      position = content_end;
    }
    // Each CSS sub-document rewinds the parent's styling cursor to its own
    // <style> body. Restore the end of the range requested by Scintilla;
    // otherwise SCI_GETENDSTYLED remains inside the first CSS block and every
    // scroll re-lexes the rest of the document.
    document->StartStyling(requested_end);
  }
  void SCI_METHOD Fold(const Sci_PositionU start, const Sci_Position length, const int style,
                       Scintilla::IDocument* document) override {
    html_->Fold(start, length, style, document);
  }
  void* SCI_METHOD PrivateCall(const int operation, void* pointer) override {
    return html_->PrivateCall(operation, pointer);
  }
  int SCI_METHOD LineEndTypesSupported() override { return html_->LineEndTypesSupported(); }
  int SCI_METHOD AllocateSubStyles(const int style, const int count) override {
    return html_->AllocateSubStyles(style, count);
  }
  int SCI_METHOD SubStylesStart(const int style) override { return html_->SubStylesStart(style); }
  int SCI_METHOD SubStylesLength(const int style) override { return html_->SubStylesLength(style); }
  int SCI_METHOD StyleFromSubStyle(const int style) override { return html_->StyleFromSubStyle(style); }
  int SCI_METHOD PrimaryStyleFromStyle(const int style) override {
    return html_->PrimaryStyleFromStyle(style);
  }
  void SCI_METHOD FreeSubStyles() override { html_->FreeSubStyles(); }
  void SCI_METHOD SetIdentifiers(const int style, const char* identifiers) override {
    html_->SetIdentifiers(style, identifiers);
  }
  int SCI_METHOD DistanceToSecondaryStyles() override {
    return html_->DistanceToSecondaryStyles();
  }
  const char* SCI_METHOD GetSubStyleBases() override { return html_->GetSubStyleBases(); }
  int SCI_METHOD NamedStyles() override { return html_->NamedStyles(); }
  const char* SCI_METHOD NameOfStyle(const int style) override { return html_->NameOfStyle(style); }
  const char* SCI_METHOD TagsOfStyle(const int style) override { return html_->TagsOfStyle(style); }
  const char* SCI_METHOD DescriptionOfStyle(const int style) override {
    return html_->DescriptionOfStyle(style);
  }
  const char* SCI_METHOD GetName() override { return "listopad-html-css"; }
  int SCI_METHOD GetIdentifier() override { return html_->GetIdentifier(); }
  const char* SCI_METHOD PropertyGet(const char* key) override { return html_->PropertyGet(key); }

 private:
  Scintilla::ILexer5* html_{};
  Scintilla::ILexer5* css_{};
};

}  // namespace

Scintilla::ILexer5* create_html_css_lexer() {
  auto* lexer = new HtmlCssLexer;
  return lexer;
}

}  // namespace listopad
