#include "listopad/ui_layout.h"

#include <algorithm>
#include <cstdint>

namespace listopad {
namespace {

int scale_for_dpi(const int value, const int dpi) noexcept {
  const auto scaled =
      static_cast<std::int64_t>(value) * std::max(1, dpi) + 48;
  return static_cast<int>(scaled / 96);
}

LayoutRect rect(const int x, const int y, const int width,
                const int height) noexcept {
  return {x, y, std::max(0, width), std::max(0, height)};
}

}  // namespace

WindowLayout calculate_window_layout(const WindowLayoutInput& input,
                                     const UiState& state) noexcept {
  const int width = std::max(0, input.client_width);
  const int height = std::max(0, input.client_height);
  const int toolbar_height = std::max(0, input.toolbar_height);
  const int status_height = std::clamp(input.status_height, 0, height);
  const int banner_height = state.banner_visible ? 36 : 0;
  const bool search_visible = state.search_mode != SearchMode::Hidden;
  const bool replace_visible = state.search_mode == SearchMode::Replace;
  const int search_controls_height = replace_visible ? 112 : 78;
  const int search_height =
      search_visible
          ? search_controls_height +
                (state.search_results_visible ? 168 : 0)
          : 0;
  const int top_offset = toolbar_height + banner_height;
  const int tabs_height =
      std::max(0, height - status_height - top_offset - search_height);
  const int search_top = height - status_height - search_height;

  WindowLayout layout;
  layout.toolbar = rect(0, 0, width, toolbar_height);
  layout.tabs = rect(0, top_offset, width, tabs_height);
  layout.banner = rect(0, toolbar_height, width, banner_height);
  layout.reload_button =
      rect(width - 260, toolbar_height + 5, 120, 26);
  layout.keep_button = rect(width - 132, toolbar_height + 5, 120, 26);
  layout.search_panel =
      rect(0, search_top, width, search_visible ? search_height : 0);
  layout.find_text =
      rect(10, search_top + 8, std::max(160, width - 270), 24);
  layout.find_button = rect(width - 250, search_top + 7, 115, 26);
  layout.find_all_button = rect(width - 125, search_top + 7, 115, 26);
  layout.regex_check = rect(10, search_top + 42, 42, 24);
  layout.case_check = rect(58, search_top + 42, 42, 24);
  layout.whole_word_check = rect(106, search_top + 42, 108, 24);
  layout.wrap_check = rect(220, search_top + 42, 92, 24);
  layout.selection_only_check = rect(318, search_top + 42, 120, 24);
  layout.all_tabs_check = rect(444, search_top + 42, 120, 24);
  layout.replace_text =
      rect(10, search_top + 76, std::max(160, width - 270), 24);
  layout.replace_button = rect(width - 250, search_top + 75, 115, 26);
  layout.replace_all_button =
      rect(width - 125, search_top + 75, 115, 26);
  layout.search_results =
      rect(10, search_top + search_controls_height,
           std::max(0, width - 20),
           state.search_results_visible
               ? std::max(0, search_height - search_controls_height - 8)
               : 0);
  return layout;
}

EditorPaneLayout calculate_editor_pane_layout(
    const LayoutRect& content, const bool document_map_visible,
    const int dpi) noexcept {
  const int desired_map_width = scale_for_dpi(168, dpi);
  const int minimum_editor_width = scale_for_dpi(160, dpi);
  const int map_width =
      document_map_visible
          ? std::min(desired_map_width,
                     std::max(0, content.width - minimum_editor_width))
          : 0;

  return {
      .editor =
          rect(content.x, content.y, content.width - map_width,
               content.height),
      .document_map =
          rect(content.x + content.width - map_width, content.y, map_width,
               content.height),
  };
}

}  // namespace listopad
