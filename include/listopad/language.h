#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace listopad {

struct LanguageInfo {
  std::string id;
  std::string lexer;
  std::string display_name;
  bool emmet_markup{false};
  bool emmet_stylesheet{false};
  std::string default_extension;
  std::vector<std::string> extensions;
};

LanguageInfo detect_language(const std::filesystem::path& path,
                             std::string_view first_line = {});
std::span<const LanguageInfo> languages();
const LanguageInfo* language_by_id(std::string_view id);
const LanguageInfo* language_by_extension(std::string_view extension);
std::filesystem::path append_default_extension(
    const std::filesystem::path& path, std::string_view language_id);

}  // namespace listopad
