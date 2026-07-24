#include "window_layout_controller.h"

#include "document_map.h"

#include <commctrl.h>

#include <algorithm>

namespace listopad::app {
namespace {

void move_window(const HWND window, const LayoutRect& rect,
                 const BOOL repaint = TRUE) {
  if (!window) return;
  MoveWindow(window, rect.x, rect.y, rect.width, rect.height, repaint);
}

}  // namespace

void WindowLayoutController::apply(
    const HWND main_window, const int toolbar_height, const int dpi,
    const int active_pane,
    const std::span<const EditorPaneWindows> panes) const {
  if (!main_window || !controls_.tabs) return;
  RECT client{};
  GetClientRect(main_window, &client);
  RECT status_rect{};
  GetWindowRect(controls_.status, &status_rect);
  const WindowLayout layout = calculate_window_layout(
      {
          .client_width = client.right - client.left,
          .client_height = client.bottom - client.top,
          .status_height = status_rect.bottom - status_rect.top,
          .toolbar_height = toolbar_height,
          .dpi = dpi,
      },
      state_);

  move_window(controls_.toolbar, layout.toolbar);
  move_window(controls_.tabs, layout.tabs);
  RECT content{0, 0, layout.tabs.width, layout.tabs.height};
  TabCtrl_AdjustRect(controls_.tabs, FALSE, &content);
  const LayoutRect editor_content{
      content.left,
      layout.tabs.y + content.top,
      std::max(0L, content.right - content.left),
      std::max(0L, content.bottom - content.top),
  };

  for (std::size_t index = 0; index < panes.size(); ++index) {
    const EditorPaneWindows& windows = panes[index];
    const EditorPaneLayout pane = calculate_editor_pane_layout(
        editor_content,
        windows.document_map_requested && windows.document_map, dpi);
    move_window(windows.editor, pane.editor);
    if (!windows.document_map) continue;
    move_window(windows.document_map, pane.document_map);
    if (pane.document_map.width > 0) {
      DocumentMap::sync(windows.document_map, windows.editor);
    }
    ShowWindow(windows.document_map,
               static_cast<int>(index) == active_pane &&
                       windows.document_map_requested &&
                       pane.document_map.width > 0
                   ? SW_SHOW
                   : SW_HIDE);
  }

  if (active_pane >= 0 &&
      active_pane < static_cast<int>(panes.size())) {
    const EditorPaneWindows& active =
        panes[static_cast<std::size_t>(active_pane)];
    if (active.editor) {
      SetWindowPos(active.editor, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (active.document_map && active.document_map_requested) {
      SetWindowPos(active.document_map, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  }

  if (state_.banner_visible) {
    SetWindowPos(controls_.banner, HWND_BOTTOM, layout.banner.x,
                 layout.banner.y, layout.banner.width, layout.banner.height,
                 SWP_NOACTIVATE);
    SetWindowPos(controls_.reload_button, HWND_TOP, layout.reload_button.x,
                 layout.reload_button.y, layout.reload_button.width,
                 layout.reload_button.height, SWP_NOACTIVATE);
    SetWindowPos(controls_.keep_button, HWND_TOP, layout.keep_button.x,
                 layout.keep_button.y, layout.keep_button.width,
                 layout.keep_button.height, SWP_NOACTIVATE);
  }

  if (state_.search_mode == SearchMode::Hidden) return;
  move_window(controls_.search_panel, layout.search_panel);
  move_window(controls_.find_text, layout.find_text);
  move_window(controls_.find_button, layout.find_button);
  move_window(controls_.find_all_button, layout.find_all_button);
  move_window(controls_.regex_check, layout.regex_check);
  move_window(controls_.case_check, layout.case_check);
  move_window(controls_.whole_word_check, layout.whole_word_check);
  move_window(controls_.wrap_check, layout.wrap_check);
  move_window(controls_.selection_only_check, layout.selection_only_check);
  move_window(controls_.all_tabs_check, layout.all_tabs_check);
  if (state_.search_mode == SearchMode::Replace) {
    move_window(controls_.replace_text, layout.replace_text);
    move_window(controls_.replace_button, layout.replace_button);
    move_window(controls_.replace_all_button, layout.replace_all_button);
  }
  if (state_.search_results_visible) {
    move_window(controls_.search_results, layout.search_results);
  }
}

}  // namespace listopad::app
