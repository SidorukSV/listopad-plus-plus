#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace listopad {

// Last non-maximized window rectangle plus the maximized flag, so the app can
// reopen where it was left. Coordinates are virtual-screen pixels; validity is
// checked against the live monitor layout on restore (see EditorWindow::create).
struct WindowBounds {
  bool valid{false};
  bool maximized{false};
  int x{0};
  int y{0};
  int width{0};
  int height{0};
};

struct Settings {
  std::string ui_language{"ru"};
  std::string theme{"system"};
  std::string font_face{"Cascadia Mono"};
  int font_size{11};
  int indent_size{2};
  bool indent_with_tabs{false};
  bool show_document_map{true};
  std::uint64_t large_file_threshold{128ull * 1024ull * 1024ull};
  std::string fallback_encoding{"auto"};
  WindowBounds window;
};

std::filesystem::path settings_path();
Settings load_settings();
bool save_settings(const Settings& settings);
bool portable_mode();

}  // namespace listopad
