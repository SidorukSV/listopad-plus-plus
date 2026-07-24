#include "document_view_controller.h"

#include "document_map.h"
#include "editor_window.h"
#include "hex_view_window.h"
#include "large_file_view.h"
#include "resource.h"
#include "listopad/file_io.h"
#include "listopad/strings.h"
#include "listopad/version.h"

#include <windows.h>

#include <optional>
#include <utility>

namespace listopad::app {

void DocumentViewController::destroy(EditorWindow&, DocumentSession& session) const
    noexcept {
  if (session.map) {
    DestroyWindow(session.map);
    session.map = nullptr;
  }
  if (session.view) {
    DestroyWindow(session.view);
    session.view = nullptr;
  }
  const EditorSurface preserved = session.take_preserved_text_surface();
  if (preserved.map) DestroyWindow(preserved.map);
  if (preserved.view) DestroyWindow(preserved.view);
}

bool DocumentViewController::create(EditorWindow& owner,
                                    DocumentSession& session) const {
  destroy(owner, session);
  switch (session.view_kind) {
    case DocumentViewKind::Text:
      session.view = owner.create_editor();
      if (!session.view) return false;
      owner.configure_editor(session.view, session.document);
      owner.set_editor_text(session, session.document.text,
                            DocumentMutationOrigin::Load);
      session.map = DocumentMap::create(owner.window_, IDC_DOCUMENT_MAP,
                                        owner.instance_);
      if (!session.map) {
        destroy(owner, session);
        return false;
      }
      DocumentMap::attach(session.map, session.view);
      DocumentMap::restyle(session.map, session.view,
                           owner.settings_.font_face, owner.dark_);
      DocumentMap::sync(session.map, session.view);
      PostMessageW(owner.window_, EditorWindow::kDocumentMapRefreshMessage,
                   reinterpret_cast<WPARAM>(session.view),
                   reinterpret_cast<LPARAM>(session.map));
      return true;
    case DocumentViewKind::LargeText:
      session.view = LargeFileView::create(owner.window_, IDC_EDITOR);
      if (!session.view) return false;
      SendMessageW(session.view, WM_SETFONT,
                   reinterpret_cast<WPARAM>(owner.editor_font_), TRUE);
      LargeFileView::set_dark(session.view, owner.dark_);
      if (!LargeFileView::open(session.view, session.document.path)) {
        destroy(owner, session);
        return false;
      }
      return true;
    case DocumentViewKind::Hex:
      session.view = HexViewWindow::create(owner.window_, IDC_EDITOR);
      if (!session.view) return false;
      SendMessageW(session.view, WM_SETFONT,
                   reinterpret_cast<WPARAM>(owner.editor_font_), TRUE);
      HexViewWindow::set_dark(session.view, owner.dark_);
      if (!HexViewWindow::open(session.view, session.document.path)) {
        destroy(owner, session);
        return false;
      }
      return true;
  }
  return false;
}

bool DocumentViewController::switch_to(
    EditorWindow& owner, DocumentSession& session,
    const DocumentViewKind requested) const {
  if (!session.document.has_path() || session.document.dirty ||
      session.document.external_diverged) {
    return false;
  }

  DocumentViewKind target = requested;
  std::optional<Document> replacement;
  if (requested == DocumentViewKind::Text &&
      session.view_kind == DocumentViewKind::Hex &&
      !session.has_preserved_text_surface()) {
    const Encoding* forced =
        session.document.large_file ? nullptr : &session.document.encoding;
    LoadDocumentResult loaded =
        load_document(session.document.path,
                      owner.settings_.large_file_threshold, forced);
    if (!loaded.ok) {
      MessageBoxW(
          owner.window_,
          (owner.tr(L"Не удалось открыть файл:\n",
                    L"Unable to open file:\n") +
           win32_error_message(loaded.error))
              .c_str(),
          LISTOPAD_PRODUCT_NAME, MB_ICONERROR);
      return false;
    }
    target = loaded.document.large_file ? DocumentViewKind::LargeText
                                        : DocumentViewKind::Text;
    replacement = std::move(loaded.document);
  }
  if (target == session.view_kind) return true;

  owner.clear_find_all_results();
  owner.search_.cancel();

  if (target == DocumentViewKind::Hex &&
      session.view_kind == DocumentViewKind::Text) {
    const HWND text_view = session.view;
    const HWND text_map = session.map;
    if (!session.preserve_text_surface()) return false;
    session.view = HexViewWindow::create(owner.window_, IDC_EDITOR);
    session.view_kind = DocumentViewKind::Hex;
    if (!session.view) {
      session.restore_text_surface();
      session.view_kind = DocumentViewKind::Text;
      return false;
    }
    SendMessageW(session.view, WM_SETFONT,
                 reinterpret_cast<WPARAM>(owner.editor_font_), TRUE);
    HexViewWindow::set_dark(session.view, owner.dark_);
    if (!HexViewWindow::open(session.view, session.document.path)) {
      DestroyWindow(session.view);
      session.view = nullptr;
      session.restore_text_surface();
      session.view_kind = DocumentViewKind::Text;
      return false;
    }
    ShowWindow(text_view, SW_HIDE);
    if (text_map) ShowWindow(text_map, SW_HIDE);
    session.clear_transient_editor_state();
    owner.refresh_view_menu_state();
    owner.update_ui();
    owner.update_layout();
    SetFocus(session.view);
    return true;
  }

  if (target == DocumentViewKind::Text &&
      session.view_kind == DocumentViewKind::Hex &&
      session.has_preserved_text_surface()) {
    const HWND hex_view = std::exchange(session.view, nullptr);
    session.restore_text_surface();
    session.view_kind = DocumentViewKind::Text;
    if (hex_view) DestroyWindow(hex_view);
    ShowWindow(session.view, SW_SHOW);
    if (session.map) {
      ShowWindow(session.map,
                 owner.settings_.show_document_map ? SW_SHOW : SW_HIDE);
    }
    owner.configure_editor(session.view, session.document);
    if (session.map) {
      DocumentMap::attach(session.map, session.view);
      DocumentMap::restyle(session.map, session.view,
                           owner.settings_.font_face, owner.dark_);
      DocumentMap::sync(session.map, session.view);
      PostMessageW(owner.window_, EditorWindow::kDocumentMapRefreshMessage,
                   reinterpret_cast<WPARAM>(session.view),
                   reinterpret_cast<LPARAM>(session.map));
    }
    owner.refresh_view_menu_state();
    owner.update_ui();
    owner.update_layout();
    SetFocus(session.view);
    return true;
  }

  const DocumentViewKind previous_kind = session.view_kind;
  const HWND previous_view = session.view;
  const HWND previous_map = session.map;
  std::optional<Document> previous_document;
  if (replacement) {
    previous_document = std::move(session.document);
    session.document = std::move(*replacement);
  }
  session.view_kind = target;
  session.view = nullptr;
  session.map = nullptr;
  if (!create(owner, session)) {
    session.view_kind = previous_kind;
    session.view = previous_view;
    session.map = previous_map;
    if (previous_document) {
      session.document = std::move(*previous_document);
    }
    MessageBoxW(owner.window_,
                owner.tr(L"Не удалось создать представление файла.",
                         L"Unable to create the file view."),
                LISTOPAD_PRODUCT_NAME, MB_ICONERROR);
    return false;
  }

  if (previous_map) DestroyWindow(previous_map);
  if (previous_view) DestroyWindow(previous_view);
  session.clear_transient_editor_state();
  owner.refresh_view_menu_state();
  owner.update_ui();
  owner.update_layout();
  SetFocus(session.view);
  return true;
}

}  // namespace listopad::app
