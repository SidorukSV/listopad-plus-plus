#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace listopad {

struct Settings {
  std::string ui_language{"ru"};
  std::string theme{"system"};
  std::string font_face{"Cascadia Mono"};
  int font_size{11};
  int indent_size{2};
  bool indent_with_tabs{false};
  std::uint64_t large_file_threshold{128ull * 1024ull * 1024ull};
  std::string fallback_encoding{"auto"};
};

std::filesystem::path settings_path();
Settings load_settings();
bool save_settings(const Settings& settings);
bool portable_mode();

}  // namespace listopad
