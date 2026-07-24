#pragma once

namespace listopad {

enum class SearchMode {
  Hidden,
  Find,
  Replace,
};

struct UiState {
  bool banner_visible{false};
  SearchMode search_mode{SearchMode::Hidden};
  bool search_results_visible{false};
};

struct LayoutRect {
  int x{0};
  int y{0};
  int width{0};
  int height{0};

  bool operator==(const LayoutRect&) const = default;
};

struct WindowLayoutInput {
  int client_width{0};
  int client_height{0};
  int status_height{0};
  int toolbar_height{0};
  int dpi{96};
};

struct WindowLayout {
  LayoutRect toolbar;
  LayoutRect tabs;
  LayoutRect banner;
  LayoutRect reload_button;
  LayoutRect keep_button;
  LayoutRect search_panel;
  LayoutRect find_text;
  LayoutRect find_button;
  LayoutRect find_all_button;
  LayoutRect regex_check;
  LayoutRect case_check;
  LayoutRect whole_word_check;
  LayoutRect wrap_check;
  LayoutRect selection_only_check;
  LayoutRect all_tabs_check;
  LayoutRect replace_text;
  LayoutRect replace_button;
  LayoutRect replace_all_button;
  LayoutRect search_results;
};

struct EditorPaneLayout {
  LayoutRect editor;
  LayoutRect document_map;
};

[[nodiscard]] WindowLayout calculate_window_layout(
    const WindowLayoutInput& input, const UiState& state) noexcept;

[[nodiscard]] EditorPaneLayout calculate_editor_pane_layout(
    const LayoutRect& content, bool document_map_visible, int dpi) noexcept;

}  // namespace listopad
