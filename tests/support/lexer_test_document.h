#pragma once

#include <ILexer.h>
#include <SciLexer.h>
#include <Scintilla.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace listopad::test {

class LexerDocument final : public Scintilla::IDocument {
 public:
  explicit LexerDocument(std::string text)
      : text_(std::move(text)), styles_(text_.size()) {}

  int SCI_METHOD Version() const override { return Scintilla::dvRelease4; }
  void SCI_METHOD SetErrorStatus(int) override {}
  Sci_Position SCI_METHOD Length() const override {
    return static_cast<Sci_Position>(text_.size());
  }
  void SCI_METHOD GetCharRange(char* buffer, const Sci_Position position,
                               const Sci_Position length) const override {
    std::memcpy(buffer, text_.data() + position,
                static_cast<std::size_t>(length));
  }
  char SCI_METHOD StyleAt(const Sci_Position position) const override {
    if (position < 0 || position >= Length()) return 0;
    return styles_[static_cast<std::size_t>(position)];
  }
  Sci_Position SCI_METHOD LineFromPosition(
      Sci_Position position) const override {
    position = (std::clamp)(position, Sci_Position{}, Length());
    return static_cast<Sci_Position>(
        std::count(text_.begin(), text_.begin() + position, '\n'));
  }
  Sci_Position SCI_METHOD LineStart(const Sci_Position line) const override {
    if (line <= 0) return 0;
    Sci_Position current = 0;
    for (Sci_Position position = 0; position < Length(); ++position) {
      if (text_[static_cast<std::size_t>(position)] == '\n' &&
          ++current == line) {
        return position + 1;
      }
    }
    return Length();
  }
  int SCI_METHOD GetLevel(const Sci_Position line) const override {
    return line >= 0 && static_cast<std::size_t>(line) < levels_.size()
               ? levels_[static_cast<std::size_t>(line)]
               : SC_FOLDLEVELBASE;
  }
  int SCI_METHOD SetLevel(const Sci_Position line, const int level) override {
    ensure_line(levels_, line, SC_FOLDLEVELBASE);
    return std::exchange(levels_[static_cast<std::size_t>(line)], level);
  }
  int SCI_METHOD GetLineState(const Sci_Position line) const override {
    return line >= 0 &&
                   static_cast<std::size_t>(line) < line_states_.size()
               ? line_states_[static_cast<std::size_t>(line)]
               : 0;
  }
  int SCI_METHOD SetLineState(const Sci_Position line,
                              const int state) override {
    ensure_line(line_states_, line, 0);
    return std::exchange(line_states_[static_cast<std::size_t>(line)], state);
  }
  void SCI_METHOD StartStyling(const Sci_Position position) override {
    styling_position_ = position;
  }
  bool SCI_METHOD SetStyleFor(const Sci_Position length,
                              const char style) override {
    const Sci_Position end =
        (std::min)(Length(), styling_position_ + length);
    std::fill(styles_.begin() + styling_position_, styles_.begin() + end,
              style);
    styling_position_ = end;
    return true;
  }
  bool SCI_METHOD SetStyles(const Sci_Position length,
                            const char* styles) override {
    const Sci_Position end =
        (std::min)(Length(), styling_position_ + length);
    std::copy(styles, styles + (end - styling_position_),
              styles_.begin() + styling_position_);
    styling_position_ = end;
    return true;
  }
  void SCI_METHOD DecorationSetCurrentIndicator(int) override {}
  void SCI_METHOD DecorationFillRange(Sci_Position, int,
                                      Sci_Position) override {}
  void SCI_METHOD ChangeLexerState(Sci_Position, Sci_Position) override {}
  int SCI_METHOD CodePage() const override { return SC_CP_UTF8; }
  bool SCI_METHOD IsDBCSLeadByte(char) const override { return false; }
  const char* SCI_METHOD BufferPointer() override { return text_.data(); }
  int SCI_METHOD GetLineIndentation(const Sci_Position line) override {
    int indentation = 0;
    for (Sci_Position position = LineStart(line); position < Length();
         ++position) {
      const char ch = text_[static_cast<std::size_t>(position)];
      if (ch == ' ') {
        ++indentation;
      } else if (ch == '\t') {
        indentation += 4;
      } else {
        break;
      }
    }
    return indentation;
  }
  Sci_Position SCI_METHOD LineEnd(const Sci_Position line) const override {
    Sci_Position position = LineStart(line);
    while (position < Length() &&
           text_[static_cast<std::size_t>(position)] != '\r' &&
           text_[static_cast<std::size_t>(position)] != '\n') {
      ++position;
    }
    return position;
  }
  Sci_Position SCI_METHOD GetRelativePosition(
      Sci_Position position,
      const Sci_Position character_offset) const override {
    if (character_offset >= 0) {
      for (Sci_Position count = 0;
           count < character_offset && position < Length(); ++count) {
        position += utf8_width(position);
      }
    } else {
      for (Sci_Position count = 0; count > character_offset && position > 0;
           --count) {
        --position;
        while (position > 0 &&
               (static_cast<unsigned char>(
                    text_[static_cast<std::size_t>(position)]) &
                0xc0) == 0x80) {
          --position;
        }
      }
    }
    return position;
  }
  int SCI_METHOD GetCharacterAndWidth(const Sci_Position position,
                                      Sci_Position* width) const override {
    if (position < 0 || position >= Length()) {
      if (width) *width = 1;
      return 0;
    }
    const unsigned char first =
        static_cast<unsigned char>(text_[static_cast<std::size_t>(position)]);
    const Sci_Position count = utf8_width(position);
    if (width) *width = count;
    if (count == 1) return first;
    int codepoint =
        first & (count == 2 ? 0x1f : count == 3 ? 0x0f : 0x07);
    for (Sci_Position index = 1; index < count; ++index) {
      codepoint =
          (codepoint << 6) |
          (static_cast<unsigned char>(
               text_[static_cast<std::size_t>(position + index)]) &
           0x3f);
    }
    return codepoint;
  }

  unsigned char style_at(const std::string& token) const {
    const std::size_t position = text_.find(token);
    assert(position != std::string::npos);
    return static_cast<unsigned char>(styles_[position]);
  }

  Sci_Position position_of(const std::string& token) const {
    const std::size_t position = text_.find(token);
    assert(position != std::string::npos);
    return static_cast<Sci_Position>(position);
  }

  void clear_styles_from(const Sci_Position position) {
    assert(position >= 0);
    assert(position <= Length());
    std::fill(styles_.begin() + position, styles_.end(), char{0});
  }

  [[nodiscard]] Sci_Position styling_position() const noexcept {
    return styling_position_;
  }
  [[nodiscard]] const std::vector<char>& styles() const noexcept {
    return styles_;
  }

 private:
  static void ensure_line(std::vector<int>& values,
                          const Sci_Position line, const int initial) {
    if (line >= 0 && static_cast<std::size_t>(line) >= values.size()) {
      values.resize(static_cast<std::size_t>(line) + 1, initial);
    }
  }

  Sci_Position utf8_width(const Sci_Position position) const {
    const unsigned char first =
        static_cast<unsigned char>(text_[static_cast<std::size_t>(position)]);
    Sci_Position width = 1;
    if ((first & 0xe0) == 0xc0) {
      width = 2;
    } else if ((first & 0xf0) == 0xe0) {
      width = 3;
    } else if ((first & 0xf8) == 0xf0) {
      width = 4;
    }
    if (position + width > Length()) return 1;
    for (Sci_Position index = 1; index < width; ++index) {
      if ((static_cast<unsigned char>(
               text_[static_cast<std::size_t>(position + index)]) &
           0xc0) != 0x80) {
        return 1;
      }
    }
    return width;
  }

  std::string text_;
  std::vector<char> styles_;
  std::vector<int> levels_;
  std::vector<int> line_states_;
  Sci_Position styling_position_{0};
};

}  // namespace listopad::test
