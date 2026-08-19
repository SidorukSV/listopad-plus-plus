#include "command_controller.h"

#include "editor_window.h"
#include "performance_log_view.h"
#include "resource.h"
#include "technology_log_view.h"
#include "listopad/settings.h"
#include "listopad/shell_registration.h"
#include "listopad/version.h"

#include <Scintilla.h>
#include <windows.h>

namespace listopad::app {
namespace {

sptr_t sci(const HWND editor, const unsigned int message,
           const uptr_t wparam = 0, const sptr_t lparam = 0) {
  return static_cast<sptr_t>(
      SendMessageW(editor, message, static_cast<WPARAM>(wparam),
                   static_cast<LPARAM>(lparam)));
}

}  // namespace

bool CommandController::dispatch(EditorWindow& owner, const int command) const {
  EditorWindow::Tab* tab = owner.active_tab();
  switch (command) {
    case IDM_FILE_NEW:
      owner.add_empty_tab();
      return true;
    case IDM_FILE_OPEN: {
      const auto path = owner.choose_open_file();
      if (!path.empty()) owner.open_file(path);
      return true;
    }
    case IDM_FILE_SAVE:
      if (tab) owner.save_tab(*tab);
      return true;
    case IDM_FILE_SAVE_AS:
      if (tab) owner.save_tab(*tab, true);
      return true;
    case IDM_FILE_CLOSE:
      owner.close_tab(owner.active_index());
      return true;
    case IDM_FILE_EXIT:
      PostMessageW(owner.window_, WM_CLOSE, 0, 0);
      return true;
    case IDM_ENCODING_UTF8:
      owner.reopen_active({EncodingKind::Utf8, CP_UTF8, false});
      return true;
    case IDM_ENCODING_UTF8_BOM:
      owner.reopen_active({EncodingKind::Utf8, CP_UTF8, true});
      return true;
    case IDM_ENCODING_UTF16LE:
      owner.reopen_active({EncodingKind::Utf16Le, 1200, true});
      return true;
    case IDM_ENCODING_UTF16BE:
      owner.reopen_active({EncodingKind::Utf16Be, 1201, true});
      return true;
    case IDM_ENCODING_CP1251:
      owner.reopen_active(
          {EncodingKind::WindowsCodePage, 1251, false});
      return true;
    case IDM_ENCODING_CP866:
      owner.reopen_active(
          {EncodingKind::WindowsCodePage, 866, false});
      return true;
    case IDM_ENCODING_CP1252:
      owner.reopen_active(
          {EncodingKind::WindowsCodePage, 1252, false});
      return true;
    case IDM_EDIT_UNDO:
      if (tab && owner.editable(*tab) && sci(tab->view, SCI_CANUNDO)) {
        sci(tab->view, SCI_UNDO);
      }
      return true;
    case IDM_EDIT_REDO:
      if (tab && owner.editable(*tab) && sci(tab->view, SCI_CANREDO)) {
        sci(tab->view, SCI_REDO);
      }
      return true;
    case IDM_EDIT_CUT:
      if (tab && owner.editable(*tab)) sci(tab->view, SCI_CUT);
      return true;
    case IDM_EDIT_COPY:
      if (tab && tab->view_kind ==
                     EditorWindow::ViewKind::TechnologyLog) {
        TechnologyLogView::copy(tab->view);
      } else if (tab && tab->view_kind ==
                            EditorWindow::ViewKind::PerformanceLog) {
        PerformanceLogView::copy(tab->view);
      } else if (tab && owner.editable(*tab)) {
        sci(tab->view, SCI_COPY);
      }
      return true;
    case IDM_EDIT_PASTE:
      if (tab && owner.editable(*tab)) sci(tab->view, SCI_PASTE);
      return true;
    case IDM_EDIT_SELECT_ALL:
      if (tab && tab->view_kind ==
                     EditorWindow::ViewKind::TechnologyLog) {
        TechnologyLogView::select_all(tab->view);
      } else if (tab && tab->view_kind ==
                            EditorWindow::ViewKind::PerformanceLog) {
        PerformanceLogView::select_all(tab->view);
      } else if (tab && owner.editable(*tab)) {
        sci(tab->view, SCI_SELECTALL);
      }
      return true;
    case IDM_SEARCH_FIND:
      owner.show_search(false);
      return true;
    case IDM_SEARCH_REPLACE:
      owner.show_search(true);
      return true;
    case IDM_SEARCH_NEXT:
    case IDC_FIND_NEXT:
      owner.find_next();
      return true;
    case IDC_FIND_ALL:
      owner.find_all();
      return true;
    case IDC_REPLACE_ALL:
      owner.replace_all_open_tabs();
      return true;
    case IDC_REPLACE_ONE:
      owner.replace_one();
      return true;
    case IDC_BANNER_RELOAD:
      owner.reload_active();
      return true;
    case IDC_BANNER_KEEP:
      owner.keep_external_active();
      return true;
    case IDM_VIEW_DOCUMENT_MAP:
      owner.settings_.show_document_map =
          !owner.settings_.show_document_map;
      save_settings(owner.settings_);
      owner.refresh_view_menu_state();
      owner.update_layout();
      return true;
    case IDM_VIEW_HEX:
      if (!tab || !tab->document.has_path() || tab->document.dirty ||
          tab->document.external_diverged) {
        MessageBeep(MB_ICONINFORMATION);
        MessageBoxW(
            owner.window_,
            owner.tr(
                L"Переключение Hex/ASCII доступно только для сохранённого "
                L"файла без локальных или внешних изменений.",
                L"Hex/ASCII switching is available only for a saved file "
                L"without local or external changes."),
            LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
      } else {
        owner.switch_tab_view(
            *tab, tab->view_kind == EditorWindow::ViewKind::Hex
                      ? EditorWindow::ViewKind::Text
                      : EditorWindow::ViewKind::Hex);
      }
      return true;
    case IDM_VIEW_TECHNOLOGY_LOG:
      if (!tab || !tab->document.has_path() || tab->document.dirty ||
          tab->document.external_diverged ||
          !tab->technology_log_candidate) {
        MessageBeep(MB_ICONINFORMATION);
        MessageBoxW(
            owner.window_,
            owner.tr(
                L"Представление ТЖ доступно только для распознанного "
                L"технологического журнала без локальных или внешних "
                L"изменений.",
                L"The technology log view is available only for a recognized "
                L"technology log without local or external changes."),
            LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
      } else {
        owner.switch_tab_view(
            *tab,
            tab->view_kind == EditorWindow::ViewKind::TechnologyLog
                ? EditorWindow::ViewKind::Text
                : EditorWindow::ViewKind::TechnologyLog);
      }
      return true;
    case IDM_VIEW_PERFORMANCE_LOG:
      if (!tab || !tab->document.has_path() || tab->document.dirty ||
          tab->document.external_diverged ||
          !tab->performance_log_candidate) {
        MessageBeep(MB_ICONINFORMATION);
        MessageBoxW(
            owner.window_,
            owner.tr(
                L"Представление системного монитора доступно только для "
                L"распознанного журнала счётчиков без локальных или внешних "
                L"изменений.",
                L"The performance log view is available only for a recognized "
                L"counter log without local or external changes."),
            LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
      } else {
        owner.switch_tab_view(
            *tab,
            tab->view_kind == EditorWindow::ViewKind::PerformanceLog
                ? EditorWindow::ViewKind::Text
                : EditorWindow::ViewKind::PerformanceLog);
      }
      return true;
    case IDM_TOOLS_FORMAT:
      owner.format_active();
      return true;
    case IDM_TOOLS_SETTINGS:
      owner.show_settings();
      return true;
    case IDM_TOOLS_REGISTER:
      MessageBoxW(
          owner.window_,
          register_classic_context_menu(owner.settings_.ui_language)
              ? owner.tr(L"Пункт контекстного меню добавлен.",
                         L"Context menu command registered.")
              : owner.tr(L"Не удалось добавить пункт контекстного меню.",
                         L"Unable to register context menu command."),
          LISTOPAD_PRODUCT_NAME, MB_OK);
      return true;
    case IDM_TOOLS_UNREGISTER:
      MessageBoxW(
          owner.window_,
          unregister_classic_context_menu()
              ? owner.tr(L"Пункт контекстного меню удалён.",
                         L"Context menu command removed.")
              : owner.tr(L"Не удалось удалить пункт контекстного меню.",
                         L"Unable to unregister context menu command."),
          LISTOPAD_PRODUCT_NAME, MB_OK);
      return true;
    case IDM_HELP_ABOUT:
      MessageBoxW(
          owner.window_,
          L"Listopad++ " LISTOPAD_VERSION_WSTRING
          L"\n\nFast offline Windows text editor\nScintilla · Lexilla · "
          L"PCRE2 · Emmet",
          LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
      return true;
    default:
      return false;
  }
}

}  // namespace listopad::app
