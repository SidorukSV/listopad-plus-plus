#include "listopad/ui_layout.h"

#include <catch2/catch_test_macros.hpp>

using namespace listopad;

TEST_CASE("window layout is derived from explicit UI state") {
  const WindowLayoutInput input{
      .client_width = 1200,
      .client_height = 900,
      .status_height = 24,
      .toolbar_height = 32,
      .dpi = 96,
  };

  const auto hidden = calculate_window_layout(input, {});
  CHECK(hidden.tabs == LayoutRect{0, 32, 1200, 844});
  CHECK(hidden.search_panel.height == 0);

  const UiState find{
      .search_mode = SearchMode::Find,
      .search_results_visible = true,
  };
  const auto find_layout = calculate_window_layout(input, find);
  CHECK(find_layout.search_panel == LayoutRect{0, 630, 1200, 246});
  CHECK(find_layout.tabs == LayoutRect{0, 32, 1200, 598});
  CHECK(find_layout.search_results.height == 160);

  const UiState replace{
      .banner_visible = true,
      .search_mode = SearchMode::Replace,
  };
  const auto replace_layout = calculate_window_layout(input, replace);
  CHECK(replace_layout.banner == LayoutRect{0, 32, 1200, 36});
  CHECK(replace_layout.tabs == LayoutRect{0, 68, 1200, 696});
  CHECK(replace_layout.replace_text.height == 24);
}

TEST_CASE("editor pane layout preserves a usable editor beside the map") {
  const LayoutRect content{7, 31, 1000, 700};
  const auto normal = calculate_editor_pane_layout(content, true, 96);
  CHECK(normal.editor == LayoutRect{7, 31, 832, 700});
  CHECK(normal.document_map == LayoutRect{839, 31, 168, 700});

  const auto narrow =
      calculate_editor_pane_layout(LayoutRect{0, 0, 200, 400}, true, 96);
  CHECK(narrow.editor.width == 160);
  CHECK(narrow.document_map.width == 40);

  const auto hidden = calculate_editor_pane_layout(content, false, 144);
  CHECK(hidden.editor == content);
  CHECK(hidden.document_map.width == 0);
}
