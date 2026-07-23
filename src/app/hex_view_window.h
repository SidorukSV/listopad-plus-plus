#pragma once

#include "listopad/search.h"

#include <windows.h>

#include <filesystem>
#include <string>

namespace listopad::app {

class HexViewWindow final {
 public:
  static bool register_class(HINSTANCE instance);
  static HWND create(HWND parent, int control_id);
  static bool open(HWND window, const std::filesystem::path& path);
  static void set_dark(HWND window, bool dark);
  static bool find_next(HWND window, std::string pattern,
                        SearchOptions options, bool wrap);

 private:
  static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                      WPARAM wparam, LPARAM lparam);
};

}  // namespace listopad::app
