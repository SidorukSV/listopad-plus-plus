#pragma once

#include "listopad/ui_layout.h"

#include <windows.h>

#include <span>

namespace listopad::app {

struct WindowControls {
  HWND toolbar{nullptr};
  HWND tabs{nullptr};
  HWND status{nullptr};
  HWND banner{nullptr};
  HWND reload_button{nullptr};
  HWND keep_button{nullptr};
  HWND search_panel{nullptr};
  HWND find_text{nullptr};
  HWND find_button{nullptr};
  HWND find_all_button{nullptr};
  HWND regex_check{nullptr};
  HWND case_check{nullptr};
  HWND whole_word_check{nullptr};
  HWND wrap_check{nullptr};
  HWND selection_only_check{nullptr};
  HWND all_tabs_check{nullptr};
  HWND replace_text{nullptr};
  HWND replace_button{nullptr};
  HWND replace_all_button{nullptr};
  HWND search_results{nullptr};
};

struct EditorPaneWindows {
  HWND editor{nullptr};
  HWND document_map{nullptr};
  bool document_map_requested{false};
};

class WindowLayoutController final {
 public:
  void bind(WindowControls controls) noexcept { controls_ = controls; }

  [[nodiscard]] UiState& state() noexcept { return state_; }
  [[nodiscard]] const UiState& state() const noexcept { return state_; }

  void apply(HWND main_window, int toolbar_height, int dpi, int active_pane,
             std::span<const EditorPaneWindows> panes) const;

 private:
  WindowControls controls_;
  UiState state_;
};

}  // namespace listopad::app
