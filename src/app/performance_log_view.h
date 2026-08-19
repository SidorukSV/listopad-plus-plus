#pragma once

#include "listopad/performance_log.h"
#include "listopad/search.h"

#include <windows.h>

#include <filesystem>
#include <string>

namespace listopad::app {

class PerformanceLogView final {
 public:
  static bool register_class(HINSTANCE instance);
  static HWND create(HWND parent, int control_id, bool russian,
                     PerformanceLogUiState initial_state);
  static bool open(HWND window, const std::filesystem::path& path);
  static bool looks_like(const std::filesystem::path& path);
  static void set_dark(HWND window, bool dark);
  static bool find_next(HWND window, const std::string& pattern,
                        SearchOptions options, bool wrap);
  static bool copy(HWND window);
  static bool select_all(HWND window);
  static PerformanceLogUiState ui_state(HWND window);

 private:
  static LRESULT CALLBACK window_proc(HWND window, UINT message,
                                      WPARAM wparam, LPARAM lparam);
};

}  // namespace listopad::app
