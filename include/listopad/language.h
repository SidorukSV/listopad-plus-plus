#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace listopad {

struct LanguageInfo {
  std::string id;
  std::string lexer;
  std::string display_name;
  bool emmet_markup{false};
  bool emmet_stylesheet{false};
};

LanguageInfo detect_language(const std::filesystem::path& path,
                             std::string_view first_line = {});
std::span<const LanguageInfo> languages();
const LanguageInfo* language_by_id(std::string_view id);

}  // namespace listopad

