#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace listopad {

enum class EncodingKind {
  Utf8,
  Utf16Le,
  Utf16Be,
  WindowsCodePage,
};

enum class EolMode { CrLf, Lf, Cr, Mixed };

struct Encoding {
  EncodingKind kind{EncodingKind::Utf8};
  unsigned code_page{65001};
  bool bom{false};

  [[nodiscard]] std::string name() const;
  friend bool operator==(const Encoding&, const Encoding&) = default;
};

struct DecodedText {
  std::string utf8;
  Encoding encoding;
  EolMode eol{EolMode::CrLf};
  bool likely_binary{false};
};

struct EncodingResult {
  bool ok{false};
  std::vector<std::byte> bytes;
  std::string error;
};

DecodedText decode_text(std::span<const std::byte> bytes,
                        const Encoding* forced = nullptr);
EncodingResult encode_text(std::string_view utf8, const Encoding& encoding);
EolMode detect_eol(std::string_view utf8);
std::string_view eol_text(EolMode mode);
Encoding encoding_from_name(std::string_view name);

}  // namespace listopad

