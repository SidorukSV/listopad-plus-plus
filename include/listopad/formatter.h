#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace listopad {

enum class FormatKind { Json, Xml, Html };

struct FormatOptions {
  std::string indent{"  "};
  std::string eol{"\r\n"};
};

struct FormatResult {
  bool ok{false};
  std::string text;
  std::string error;
  std::size_t line{0};
  std::size_t column{0};
};

FormatResult format_text(FormatKind kind, std::string_view input,
                         const FormatOptions& options = {});

}  // namespace listopad

