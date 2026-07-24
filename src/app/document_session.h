#pragma once

#include "listopad/document.h"
#include "listopad/emmet_engine.h"

#include <windows.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace listopad::app {

enum class DocumentViewKind {
  Text,
  LargeText,
  Hex,
};

struct EditorSurface {
  HWND view{nullptr};
  HWND map{nullptr};
};

class DocumentSession final {
 public:
  Document document;
  DocumentViewKind view_kind{DocumentViewKind::Text};
  HWND view{nullptr};
  HWND map{nullptr};
  bool external_notice_pending{false};
  std::vector<EmmetField> snippet_fields;
  std::size_t snippet_index{0};

  [[nodiscard]] bool editable() const noexcept {
    return view_kind == DocumentViewKind::Text;
  }

  [[nodiscard]] bool owns_view(const HWND candidate) const noexcept {
    return candidate &&
           (view == candidate || preserved_text_.view == candidate);
  }

  [[nodiscard]] bool has_preserved_text_surface() const noexcept {
    return preserved_text_.view != nullptr;
  }

  [[nodiscard]] HWND preserved_text_view() const noexcept {
    return preserved_text_.view;
  }

  [[nodiscard]] bool preserve_text_surface() noexcept {
    if (has_preserved_text_surface() || !view) return false;
    preserved_text_ = {.view = std::exchange(view, nullptr),
                       .map = std::exchange(map, nullptr)};
    return true;
  }

  bool restore_text_surface() noexcept {
    if (!has_preserved_text_surface() || view || map) return false;
    view = std::exchange(preserved_text_.view, nullptr);
    map = std::exchange(preserved_text_.map, nullptr);
    return true;
  }

  [[nodiscard]] EditorSurface take_preserved_text_surface() noexcept {
    return std::exchange(preserved_text_, {});
  }

  void clear_transient_editor_state() noexcept {
    snippet_fields.clear();
    snippet_index = 0;
  }

 private:
  EditorSurface preserved_text_;
};

}  // namespace listopad::app
